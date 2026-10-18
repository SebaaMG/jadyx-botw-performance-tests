/*
 * SPDX-FileCopyrightText: Copyright 2026 SeriesX private project contributors
 * SPDX-License-Identifier: MIT
 *
 * Persistent descriptor arrays for SM 6.0-6.4, using ordinary descriptor tables.
 * This does not use createHandleFromHeap (SM 6.6). The original guest binding
 * selects a row in b0 space 28; that row selects a stable descriptor array slot.
 * Separate register spaces avoid overlapping typed declarations in one shader.
 */
#include "nir_builder.h"
#include "eden_spirv_to_dxil.h"

struct eden_persistent_var {
   nir_variable *original;
   nir_variable *array;
   unsigned binding;
   bool descriptor_array;
   bool sampler;
};

struct eden_persistent_state {
   struct eden_persistent_var *vars;
   unsigned count;
   bool valid;
};

static nir_def *
eden_persistent_index(nir_builder *b, unsigned binding, bool sampler)
{
   nir_def *index = nir_vulkan_resource_index(b, 2, 32, nir_imm_int(b, 0),
      .desc_set = EDEN_PERSISTENT_INDEX_SPACE, .binding = 0,
      .desc_type = nir_descriptor_type_uniform_buffer);
   nir_def *descriptor = nir_load_vulkan_descriptor(b, 2, 32, index,
      .desc_type = nir_descriptor_type_uniform_buffer);
   const unsigned offset = binding * 16 + (sampler ? 4 : 0);
   return nir_load_ubo(b, 1, 32, nir_channel(b, descriptor, 0),
      nir_imm_int(b, offset), .align_mul = 4, .align_offset = 0,
      .range_base = offset, .range = 4);
}

static struct eden_persistent_var *
eden_find_persistent_var(struct eden_persistent_state *s, nir_variable *var)
{
   for (unsigned i = 0; i < s->count; ++i)
      if (s->vars[i].original == var)
         return &s->vars[i];
   return NULL;
}

static bool
eden_persistent_instr(nir_builder *b, nir_instr *instr, void *data)
{
   struct eden_persistent_state *s = data;
   if (instr->type == nir_instr_type_intrinsic) {
      nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
      if (intr->intrinsic != nir_intrinsic_vulkan_resource_index ||
          nir_intrinsic_desc_set(intr) != 0)
         return false;
      const unsigned binding = nir_intrinsic_binding(intr);
      /* Descriptor arrays are selected out before this variant is compiled.
       * Do not silently turn an out-of-range or indirect guest index into a
       * reference to a different binding's row. */
      if (!nir_src_is_const(intr->src[0]) || nir_src_as_uint(intr->src[0]) != 0) {
         s->valid = false;
         return false;
      }
      const bool ubo = nir_intrinsic_desc_type(intr) == nir_descriptor_type_uniform_buffer;
      struct eden_persistent_var *entry = NULL;
      for (unsigned i = 0; i < s->count; ++i) {
         if (s->vars[i].binding == binding &&
             s->vars[i].original->data.mode == (ubo ? nir_var_mem_ubo : nir_var_mem_ssbo)) {
            entry = &s->vars[i];
            break;
         }
      }
      if (!entry) {
         s->valid = false;
         return false;
      }
      b->cursor = nir_before_instr(instr);
      nir_src_rewrite(&intr->src[0], eden_persistent_index(b, binding, false));
      nir_intrinsic_set_binding(intr, 0);
      nir_intrinsic_set_desc_set(intr, entry->array->data.descriptor_set);
      return true;
   }
   if (instr->type != nir_instr_type_deref)
      return false;
   nir_deref_instr *deref = nir_instr_as_deref(instr);
   if (deref->deref_type == nir_deref_type_var) {
      struct eden_persistent_var *entry = eden_find_persistent_var(s, deref->var);
      if (!entry || entry->descriptor_array)
         return false;
      b->cursor = nir_before_instr(instr);
      nir_deref_instr *replacement = nir_build_deref_array(b,
         nir_build_deref_var(b, entry->array),
         eden_persistent_index(b, entry->binding, entry->sampler));
      nir_def_rewrite_uses(&deref->def, &replacement->def);
      return true;
   }
   if (deref->deref_type == nir_deref_type_array) {
      nir_deref_instr *parent = nir_deref_instr_parent(deref);
      if (!parent || parent->deref_type != nir_deref_type_var)
         return false;
      struct eden_persistent_var *entry = eden_find_persistent_var(s, parent->var);
      if (!entry || !entry->descriptor_array)
         return false;
      if (!nir_src_is_const(deref->arr.index) || nir_src_as_uint(deref->arr.index) != 0) {
         s->valid = false;
         return false;
      }
      b->cursor = nir_before_instr(instr);
      nir_deref_instr *replacement = nir_build_deref_array(b,
         nir_build_deref_var(b, entry->array),
         eden_persistent_index(b, entry->binding, entry->sampler));
      nir_def_rewrite_uses(&deref->def, &replacement->def);
      return true;
   }
   return false;
}

static bool
eden_lower_persistent_bindings(nir_shader *nir)
{
   const nir_variable_mode modes = nir_var_mem_ubo | nir_var_mem_ssbo |
                                  nir_var_uniform | nir_var_image;
   unsigned count = 0;
   nir_foreach_variable_with_modes(var, nir, modes) {
      if (var->data.descriptor_set == 0)
         ++count;
   }
   struct eden_persistent_state state = {
      .vars = rzalloc_array(NULL, struct eden_persistent_var, count),
      .count = count, .valid = true,
   };
   if (count && !state.vars)
      return false;
   unsigned i = 0;
   nir_foreach_variable_with_modes(var, nir, modes) {
      if (var->data.descriptor_set != 0)
         continue;
      if (var->data.binding >= EDEN_PERSISTENT_MAX_BINDINGS ||
          (glsl_type_is_array(var->type) && glsl_get_aoa_size(var->type) != 1)) {
         ralloc_free(state.vars);
         return false;
      }
      state.vars[i++] = (struct eden_persistent_var){
         .original = var, .binding = var->data.binding,
         .descriptor_array = glsl_type_is_array(var->type),
         .sampler = glsl_type_is_bare_sampler(glsl_without_array(var->type)),
      };
   }
   nir_variable *groups[EDEN_PERSISTENT_INDEX_SPACE] = {0};
   unsigned next_typed_space = 3;
   for (i = 0; i < count; ++i) {
      struct eden_persistent_var *entry = &state.vars[i];
      nir_variable *old = entry->original;
      const struct glsl_type *type = glsl_without_array(old->type);
      unsigned space;
      if (entry->sampler) {
         space = glsl_sampler_type_is_shadow(type) ? EDEN_PERSISTENT_SHADOW_SPACE
                                                 : EDEN_PERSISTENT_SAMPLER_SPACE;
      } else if (old->data.mode == nir_var_mem_ubo) {
         space = 0;
      } else if (old->data.mode == nir_var_mem_ssbo) {
         space = (old->data.access & ACCESS_NON_WRITEABLE) ? 1 : 2;
      } else {
         for (space = 3; space < next_typed_space; ++space) {
            nir_variable *candidate = groups[space];
            if (candidate && glsl_without_array(candidate->type) == type &&
                candidate->data.mode == old->data.mode &&
                candidate->data.access == old->data.access)
               break;
         }
         if (space == next_typed_space)
            ++next_typed_space;
         if (space >= EDEN_PERSISTENT_RESOURCE_SPACES) {
            ralloc_free(state.vars);
            return false;
         }
      }
      if (!groups[space]) {
         nir_variable *array = nir_variable_clone(old, nir);
         array->type = glsl_array_type(type, entry->sampler ? EDEN_PERSISTENT_SAMPLER_CAPACITY
                                                          : EDEN_PERSISTENT_RESOURCE_CAPACITY, 0);
         array->data.binding = 0;
         array->data.descriptor_set = space;
         nir_shader_add_variable(nir, array);
         groups[space] = array;
      }
      entry->array = groups[space];
   }
   nir_shader_instructions_pass(nir, eden_persistent_instr, nir_metadata_none, &state);
   if (!state.valid) {
      ralloc_free(state.vars);
      return false;
   }
   nir_remove_dead_derefs(nir);
   for (i = 0; i < count; ++i)
      exec_node_remove(&state.vars[i].original->node);
   ralloc_free(state.vars);

   const struct glsl_type *rows = glsl_array_type(glsl_uvec4_type(),
                                                EDEN_PERSISTENT_MAX_BINDINGS, 16);
   const struct glsl_struct_field field = {.type = rows, .name = "rows"};
   nir_variable *map = nir_variable_create(nir, nir_var_mem_ubo,
      glsl_struct_type(&field, 1, "persistent_indices", false), "persistent_indices");
   map->data.binding = 0;
   map->data.descriptor_set = EDEN_PERSISTENT_INDEX_SPACE;
   map->data.how_declared = nir_var_hidden;
   nir_validate_shader(nir, "SeriesX persistent descriptor arrays");
   return true;
}
