/**************************************************************************

Copyright 2002-2008 VMware, Inc.

All Rights Reserved.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
on the rights to use, copy, modify, merge, publish, distribute, sub
license, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice (including the next
paragraph) shall be included in all copies or substantial portions of the
Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
VMWARE AND/OR THEIR SUPPLIERS BE LIABLE FOR ANY CLAIM,
DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
USE OR OTHER DEALINGS IN THE SOFTWARE.

**************************************************************************/

/*
 * Authors:
 *   Keith Whitwell <keithw@vmware.com>
 */



/* Display list compiler attempts to store lists of vertices with the
 * same vertex layout.  Additionally it attempts to minimize the need
 * for execute-time fixup of these vertex lists, allowing them to be
 * cached on hardware.
 *
 * There are still some circumstances where this can be thwarted, for
 * example by building a list that consists of one very long primitive
 * (eg Begin(Triangles), 1000 vertices, End), and calling that list
 * from inside a different begin/end object (Begin(Lines), CallList,
 * End).
 *
 * In that case the code will have to replay the list as individual
 * commands through the Exec dispatch table, or fix up the copied
 * vertices at execute-time.
 *
 * The other case where fixup is required is when a vertex attribute
 * is introduced in the middle of a primitive.  Eg:
 *  Begin(Lines)
 *  TexCoord1f()           Vertex2f()
 *  TexCoord1f() Color3f() Vertex2f()
 *  End()
 *
 *  If the current value of Color isn't known at compile-time, this
 *  primitive will require fixup.
 *
 *
 * The list compiler currently doesn't attempt to compile lists
 * containing EvalCoord or EvalPoint commands.  On encountering one of
 * these, compilation falls back to opcodes.
 *
 * This could be improved to fallback only when a mix of EvalCoord and
 * Vertex commands are issued within a single primitive.
 */


#include "main/glheader.h"
#include "main/arrayobj.h"
#include "main/bufferobj.h"
#include "main/context.h"
#include "main/dlist.h"
#include "main/enums.h"
#include "main/eval.h"
#include "main/macros.h"
#include "main/draw_validate.h"
#include "main/api_arrayelt.h"
#include "main/vtxfmt.h"
#include "main/dispatch.h"
#include "main/state.h"
#include "main/varray.h"
#include "util/bitscan.h"

#include "vbo_noop.h"
#include "vbo_private.h"


#ifdef ERROR
#undef ERROR
#endif

/**
 * Display list flag only used by this VBO code.
 */
#define DLIST_DANGLING_REFS     0x1


/* An interesting VBO number/name to help with debugging */
#define VBO_BUF_ID  12345


/*
 * NOTE: Old 'parity' issue is gone, but copying can still be
 * wrong-footed on replay.
 */
static GLuint
copy_vertices(struct gl_context *ctx,
              const struct vbo_save_vertex_list *node,
              const fi_type * src_buffer)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   const struct _mesa_prim *prim = &node->prims[node->prim_count - 1];
   GLuint nr = prim->count;
   GLuint sz = save->vertex_size;
   const fi_type *src = src_buffer + prim->start * sz;
   fi_type *dst = save->copied.buffer;
   GLuint ovf, i;

   if (prim->end)
      return 0;

   switch (prim->mode) {
   case GL_POINTS:
      return 0;
   case GL_LINES:
      ovf = nr & 1;
      for (i = 0; i < ovf; i++)
         memcpy(dst + i * sz, src + (nr - ovf + i) * sz,
                sz * sizeof(GLfloat));
      return i;
   case GL_TRIANGLES:
      ovf = nr % 3;
      for (i = 0; i < ovf; i++)
         memcpy(dst + i * sz, src + (nr - ovf + i) * sz,
                sz * sizeof(GLfloat));
      return i;
   case GL_QUADS:
      ovf = nr & 3;
      for (i = 0; i < ovf; i++)
         memcpy(dst + i * sz, src + (nr - ovf + i) * sz,
                sz * sizeof(GLfloat));
      return i;
   case GL_LINE_STRIP:
      if (nr == 0)
         return 0;
      else {
         memcpy(dst, src + (nr - 1) * sz, sz * sizeof(GLfloat));
         return 1;
      }
   case GL_LINE_LOOP:
   case GL_TRIANGLE_FAN:
   case GL_POLYGON:
      if (nr == 0)
         return 0;
      else if (nr == 1) {
         memcpy(dst, src + 0, sz * sizeof(GLfloat));
         return 1;
      }
      else {
         memcpy(dst, src + 0, sz * sizeof(GLfloat));
         memcpy(dst + sz, src + (nr - 1) * sz, sz * sizeof(GLfloat));
         return 2;
      }
   case GL_TRIANGLE_STRIP:
   case GL_QUAD_STRIP:
      switch (nr) {
      case 0:
         ovf = 0;
         break;
      case 1:
         ovf = 1;
         break;
      default:
         ovf = 2 + (nr & 1);
         break;
      }
      for (i = 0; i < ovf; i++)
         memcpy(dst + i * sz, src + (nr - ovf + i) * sz,
                sz * sizeof(GLfloat));
      return i;
   default:
      unreachable("Unexpected primitive type");
      return 0;
   }
}


static struct vbo_save_vertex_store *
alloc_vertex_store(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   struct vbo_save_vertex_store *vertex_store =
      CALLOC_STRUCT(vbo_save_vertex_store);

   /* obj->Name needs to be non-zero, but won't ever be examined more
    * closely than that.  In particular these buffers won't be entered
    * into the hash and can never be confused with ones visible to the
    * user.  Perhaps there could be a special number for internal
    * buffers:
    */
   vertex_store->bufferobj = ctx->Driver.NewBufferObject(ctx, VBO_BUF_ID);
   if (vertex_store->bufferobj) {
      save->out_of_memory =
         !ctx->Driver.BufferData(ctx,
                                 GL_ARRAY_BUFFER_ARB,
                                 VBO_SAVE_BUFFER_SIZE * sizeof(GLfloat),
                                 NULL, GL_STATIC_DRAW_ARB,
                                 GL_MAP_WRITE_BIT |
                                 GL_DYNAMIC_STORAGE_BIT,
                                 vertex_store->bufferobj);
   }
   else {
      save->out_of_memory = GL_TRUE;
   }

   if (save->out_of_memory) {
      _mesa_error(ctx, GL_OUT_OF_MEMORY, "internal VBO allocation");
      _mesa_install_save_vtxfmt(ctx, &save->vtxfmt_noop);
   }

   vertex_store->buffer_map = NULL;
   vertex_store->used = 0;

   return vertex_store;
}


static void
free_vertex_store(struct gl_context *ctx,
                  struct vbo_save_vertex_store *vertex_store)
{
   assert(!vertex_store->buffer_map);

   if (vertex_store->bufferobj) {
      _mesa_reference_buffer_object(ctx, &vertex_store->bufferobj, NULL);
   }

   free(vertex_store);
}


fi_type *
vbo_save_map_vertex_store(struct gl_context *ctx,
                          struct vbo_save_vertex_store *vertex_store)
{
   const GLbitfield access = (GL_MAP_WRITE_BIT |
                              GL_MAP_INVALIDATE_RANGE_BIT |
                              GL_MAP_UNSYNCHRONIZED_BIT |
                              GL_MAP_FLUSH_EXPLICIT_BIT);

   assert(vertex_store->bufferobj);
   assert(!vertex_store->buffer_map);  /* the buffer should not be mapped */

   if (vertex_store->bufferobj->Size > 0) {
      /* Map the remaining free space in the VBO */
      GLintptr offset = vertex_store->used * sizeof(GLfloat);
      GLsizeiptr size = vertex_store->bufferobj->Size - offset;
      fi_type *range = (fi_type *)
         ctx->Driver.MapBufferRange(ctx, offset, size, access,
                                    vertex_store->bufferobj,
                                    MAP_INTERNAL);
      if (range) {
         /* compute address of start of whole buffer (needed elsewhere) */
         vertex_store->buffer_map = range - vertex_store->used;
         assert(vertex_store->buffer_map);
         return range;
      }
      else {
         vertex_store->buffer_map = NULL;
         return NULL;
      }
   }
   else {
      /* probably ran out of memory for buffers */
      return NULL;
   }
}


void
vbo_save_unmap_vertex_store(struct gl_context *ctx,
                            struct vbo_save_vertex_store *vertex_store)
{
   if (vertex_store->bufferobj->Size > 0) {
      GLintptr offset = 0;
      GLsizeiptr length = vertex_store->used * sizeof(GLfloat)
         - vertex_store->bufferobj->Mappings[MAP_INTERNAL].Offset;

      /* Explicitly flush the region we wrote to */
      ctx->Driver.FlushMappedBufferRange(ctx, offset, length,
                                         vertex_store->bufferobj,
                                         MAP_INTERNAL);

      ctx->Driver.UnmapBuffer(ctx, vertex_store->bufferobj, MAP_INTERNAL);
   }
   vertex_store->buffer_map = NULL;
}


static struct vbo_save_primitive_store *
alloc_prim_store(void)
{
   struct vbo_save_primitive_store *store =
      CALLOC_STRUCT(vbo_save_primitive_store);
   store->used = 0;
   store->refcount = 1;
   return store;
}


static void
reset_counters(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   save->prims = save->prim_store->prims + save->prim_store->used;
   save->buffer_map = save->vertex_store->buffer_map + save->vertex_store->used;

   assert(save->buffer_map == save->buffer_ptr);

   if (save->vertex_size)
      save->max_vert = (VBO_SAVE_BUFFER_SIZE - save->vertex_store->used) /
                        save->vertex_size;
   else
      save->max_vert = 0;

   save->vert_count = 0;
   save->prim_count = 0;
   save->prim_max = VBO_SAVE_PRIM_SIZE - save->prim_store->used;
   save->dangling_attr_ref = GL_FALSE;
}

/**
 * For a list of prims, try merging prims that can just be extensions of the
 * previous prim.
 */
static void
merge_prims(struct _mesa_prim *prim_list,
            GLuint *prim_count)
{
   GLuint i;
   struct _mesa_prim *prev_prim = prim_list;

   for (i = 1; i < *prim_count; i++) {
      struct _mesa_prim *this_prim = prim_list + i;

      vbo_try_prim_conversion(this_prim);

      if (vbo_can_merge_prims(prev_prim, this_prim)) {
         /* We've found a prim that just extend the previous one.  Tack it
          * onto the previous one, and let this primitive struct get dropped.
          */
         vbo_merge_prims(prev_prim, this_prim);
         continue;
      }

      /* If any previous primitives have been dropped, then we need to copy
       * this later one into the next available slot.
       */
      prev_prim++;
      if (prev_prim != this_prim)
         *prev_prim = *this_prim;
   }

   *prim_count = prev_prim - prim_list + 1;
}


/**
 * Convert GL_LINE_LOOP primitive into GL_LINE_STRIP so that drivers
 * don't have to worry about handling the _mesa_prim::begin/end flags.
 * See https://bugs.freedesktop.org/show_bug.cgi?id=81174
 */
static void
convert_line_loop_to_strip(struct vbo_save_context *save,
                           struct vbo_save_vertex_list *node)
{
   struct _mesa_prim *prim = &node->prims[node->prim_count - 1];

   assert(prim->mode == GL_LINE_LOOP);

   if (prim->end) {
      /* Copy the 0th vertex to end of the buffer and extend the
       * vertex count by one to finish the line loop.
       */
      const GLuint sz = save->vertex_size;
      /* 0th vertex: */
      const fi_type *src = save->buffer_map + prim->start * sz;
      /* end of buffer: */
      fi_type *dst = save->buffer_map + (prim->start + prim->count) * sz;

      memcpy(dst, src, sz * sizeof(float));

      prim->count++;
      node->vertex_count++;
      save->vert_count++;
      save->buffer_ptr += sz;
      save->vertex_store->used += sz;
   }

   if (!prim->begin) {
      /* Drawing the second or later section of a long line loop.
       * Skip the 0th vertex.
       */
      prim->start++;
      prim->count--;
   }

   prim->mode = GL_LINE_STRIP;
}


/* Compare the present vao if it has the same setup. */
static bool
compare_vao(gl_vertex_processing_mode mode,
            const struct gl_vertex_array_object *vao,
            const struct gl_buffer_object *bo, GLintptr buffer_offset,
            GLuint stride, GLbitfield64 vao_enabled,
            const GLubyte size[VBO_ATTRIB_MAX],
            const GLenum16 type[VBO_ATTRIB_MAX],
            const GLuint offset[VBO_ATTRIB_MAX])
{
   if (!vao)
      return false;

   /* If the enabled arrays are not the same we are not equal. */
   if (vao_enabled != vao->_Enabled)
      return false;

   /* Check the buffer binding at 0 */
   if (vao->BufferBinding[0].BufferObj != bo)
      return false;
   /* BufferBinding[0].Offset != buffer_offset is checked per attribute */
   if (vao->BufferBinding[0].Stride != stride)
      return false;
   assert(vao->BufferBinding[0].InstanceDivisor == 0);

   /* Retrieve the mapping from VBO_ATTRIB to VERT_ATTRIB space */
   const GLubyte *const vao_to_vbo_map = _vbo_attribute_alias_map[mode];

   /* Now check the enabled arrays */
   GLbitfield mask = vao_enabled;
   while (mask) {
      const int attr = u_bit_scan(&mask);
      const unsigned char vbo_attr = vao_to_vbo_map[attr];
      const GLenum16 tp = type[vbo_attr];
      const GLintptr off = offset[vbo_attr] + buffer_offset;
      const struct gl_array_attributes *attrib = &vao->VertexAttrib[attr];
      if (attrib->RelativeOffset + vao->BufferBinding[0].Offset != off)
         return false;
      if (attrib->Type != tp)
         return false;
      if (attrib->Size != size[vbo_attr])
         return false;
      assert(attrib->Format == GL_RGBA);
      assert(attrib->Enabled == GL_TRUE);
      assert(attrib->Normalized == GL_FALSE);
      assert(attrib->Integer == vbo_attrtype_to_integer_flag(tp));
      assert(attrib->Doubles == vbo_attrtype_to_double_flag(tp));
      assert(attrib->BufferBindingIndex == 0);
   }

   return true;
}


/* Create or reuse the vao for the vertex processing mode. */
static void
update_vao(struct gl_context *ctx,
           gl_vertex_processing_mode mode,
           struct gl_vertex_array_object **vao,
           struct gl_buffer_object *bo, GLintptr buffer_offset,
           GLuint stride, GLbitfield64 vbo_enabled,
           const GLubyte size[VBO_ATTRIB_MAX],
           const GLenum16 type[VBO_ATTRIB_MAX],
           const GLuint offset[VBO_ATTRIB_MAX])
{
   /* Compute the bitmasks of vao_enabled arrays */
   GLbitfield vao_enabled = _vbo_get_vao_enabled_from_vbo(mode, vbo_enabled);

   /*
    * Check if we can possibly reuse the exisiting one.
    * In the long term we should reset them when something changes.
    */
   if (compare_vao(mode, *vao, bo, buffer_offset, stride,
                   vao_enabled, size, type, offset))
      return;

   /* The initial refcount is 1 */
   _mesa_reference_vao(ctx, vao, NULL);
   *vao = _mesa_new_vao(ctx, ~((GLuint)0));

   /*
    * assert(stride <= ctx->Const.MaxVertexAttribStride);
    * MaxVertexAttribStride is not set for drivers that does not
    * expose GL 44 or GLES 31.
    */

   /* Bind the buffer object at binding point 0 */
   _mesa_bind_vertex_buffer(ctx, *vao, 0, bo, buffer_offset, stride);

   /* Retrieve the mapping from VBO_ATTRIB to VERT_ATTRIB space
    * Note that the position/generic0 aliasing is done in the VAO.
    */
   const GLubyte *const vao_to_vbo_map = _vbo_attribute_alias_map[mode];
   /* Now set the enable arrays */
   GLbitfield mask = vao_enabled;
   while (mask) {
      const int vao_attr = u_bit_scan(&mask);
      const GLubyte vbo_attr = vao_to_vbo_map[vao_attr];
      assert(offset[vbo_attr] <= ctx->Const.MaxVertexAttribRelativeOffset);

      _vbo_set_attrib_format(ctx, *vao, vao_attr, buffer_offset,
                             size[vbo_attr], type[vbo_attr], offset[vbo_attr]);
      _mesa_vertex_attrib_binding(ctx, *vao, vao_attr, 0);
      _mesa_enable_vertex_array_attrib(ctx, *vao, vao_attr);
   }
   assert(vao_enabled == (*vao)->_Enabled);
   assert((vao_enabled & ~(*vao)->VertexAttribBufferMask) == 0);

   /* Finalize and freeze the VAO */
   _mesa_set_vao_immutable(ctx, *vao);
}


/**
 * Insert the active immediate struct onto the display list currently
 * being built.
 */
static void
compile_vertex_list(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   struct vbo_save_vertex_list *node;

   /* Allocate space for this structure in the display list currently
    * being compiled.
    */
   node = (struct vbo_save_vertex_list *)
      _mesa_dlist_alloc_aligned(ctx, save->opcode_vertex_list, sizeof(*node));

   if (!node)
      return;

   /* Make sure the pointer is aligned to the size of a pointer */
   assert((GLintptr) node % sizeof(void *) == 0);

   /* Duplicate our template, increment refcounts to the storage structs:
    */
   GLintptr old_offset = 0;
   if (save->VAO[0]) {
      old_offset = save->VAO[0]->BufferBinding[0].Offset
         + save->VAO[0]->VertexAttrib[VERT_ATTRIB_POS].RelativeOffset;
   }
   const GLsizei stride = save->vertex_size*sizeof(GLfloat);
   GLintptr buffer_offset =
       (save->buffer_map - save->vertex_store->buffer_map) * sizeof(GLfloat);
   assert(old_offset <= buffer_offset);
   const GLintptr offset_diff = buffer_offset - old_offset;
   GLuint start_offset = 0;
   if (offset_diff > 0 && stride > 0 && offset_diff % stride == 0) {
      /* The vertex size is an exact multiple of the buffer offset.
       * This means that we can use zero-based vertex attribute pointers
       * and specify the start of the primitive with the _mesa_prim::start
       * field.  This results in issuing several draw calls with identical
       * vertex attribute information.  This can result in fewer state
       * changes in drivers.  In particular, the Gallium CSO module will
       * filter out redundant vertex buffer changes.
       */
      /* We cannot immediately update the primitives as some methods below
       * still need the uncorrected start vertices
       */
      start_offset = offset_diff/stride;
      assert(old_offset == buffer_offset - offset_diff);
      buffer_offset = old_offset;
   }
   GLuint offsets[VBO_ATTRIB_MAX];
   for (unsigned i = 0, offset = 0; i < VBO_ATTRIB_MAX; ++i) {
      offsets[i] = offset;
      offset += save->attrsz[i] * sizeof(GLfloat);
   }
   node->vertex_count = save->vert_count;
   node->wrap_count = save->copied.nr;
   node->prims = save->prims;
   node->prim_count = save->prim_count;
   node->prim_store = save->prim_store;

   /* Create a pair of VAOs for the possible VERTEX_PROCESSING_MODEs
    * Note that this may reuse the previous one of possible.
    */
   for (gl_vertex_processing_mode vpm = VP_MODE_FF; vpm < VP_MODE_MAX; ++vpm) {
      /* create or reuse the vao */
      update_vao(ctx, vpm, &save->VAO[vpm],
                 save->vertex_store->bufferobj, buffer_offset, stride,
                 save->enabled, save->attrsz, save->attrtype, offsets);
      /* Reference the vao in the dlist */
      node->VAO[vpm] = NULL;
      _mesa_reference_vao(ctx, &node->VAO[vpm], save->VAO[vpm]);
   }

   node->prim_store->refcount++;

   if (node->prims[0].no_current_update) {
      node->current_data = NULL;
   }
   else {
      GLuint current_size = save->vertex_size - save->attrsz[0];
      node->current_data = NULL;

      if (current_size) {
         node->current_data = malloc(current_size * sizeof(GLfloat));
         if (node->current_data) {
            const char *buffer = (const char *)save->buffer_map;
            unsigned attr_offset = save->attrsz[0] * sizeof(GLfloat);
            unsigned vertex_offset = 0;

            if (node->vertex_count)
               vertex_offset = (node->vertex_count - 1) * stride;

            memcpy(node->current_data, buffer + vertex_offset + attr_offset,
                   current_size * sizeof(GLfloat));
         } else {
            _mesa_error(ctx, GL_OUT_OF_MEMORY, "Current value allocation");
         }
      }
   }

   assert(save->attrsz[VBO_ATTRIB_POS] != 0 || node->vertex_count == 0);

   if (save->dangling_attr_ref)
      ctx->ListState.CurrentList->Flags |= DLIST_DANGLING_REFS;

   save->vertex_store->used += save->vertex_size * node->vertex_count;
   save->prim_store->used += node->prim_count;

   /* Copy duplicated vertices
    */
   save->copied.nr = copy_vertices(ctx, node, save->buffer_map);

   if (node->prims[node->prim_count - 1].mode == GL_LINE_LOOP) {
      convert_line_loop_to_strip(save, node);
   }

   merge_prims(node->prims, &node->prim_count);

   /* Correct the primitive starts, we can only do this here as copy_vertices
    * and convert_line_loop_to_strip above consume the uncorrected starts.
    * On the other hand the _vbo_loopback_vertex_list call below needs the
    * primitves to be corrected already.
    */
   for (unsigned i = 0; i < node->prim_count; i++) {
      node->prims[i].start += start_offset;
   }

   /* Deal with GL_COMPILE_AND_EXECUTE:
    */
   if (ctx->ExecuteFlag) {
      struct _glapi_table *dispatch = GET_DISPATCH();

      _glapi_set_dispatch(ctx->Exec);

      /* Note that the range of referenced vertices must be mapped already */
      _vbo_loopback_vertex_list(ctx, node);

      _glapi_set_dispatch(dispatch);
   }

   /* Decide whether the storage structs are full, or can be used for
    * the next vertex lists as well.
    */
   if (save->vertex_store->used >
       VBO_SAVE_BUFFER_SIZE - 16 * (save->vertex_size + 4)) {

      /* Unmap old store:
       */
      vbo_save_unmap_vertex_store(ctx, save->vertex_store);

      /* Release old reference:
       */
      free_vertex_store(ctx, save->vertex_store);
      save->vertex_store = NULL;
      /* When we have a new vbo, we will for sure need a new vao */
      for (gl_vertex_processing_mode vpm = 0; vpm < VP_MODE_MAX; ++vpm)
         _mesa_reference_vao(ctx, &save->VAO[vpm], NULL);

      /* Allocate and map new store:
       */
      save->vertex_store = alloc_vertex_store(ctx);
      save->buffer_ptr = vbo_save_map_vertex_store(ctx, save->vertex_store);
      save->out_of_memory = save->buffer_ptr == NULL;
   }
   else {
      /* update buffer_ptr for next vertex */
      save->buffer_ptr = save->vertex_store->buffer_map
         + save->vertex_store->used;
   }

   if (save->prim_store->used > VBO_SAVE_PRIM_SIZE - 6) {
      save->prim_store->refcount--;
      assert(save->prim_store->refcount != 0);
      save->prim_store = alloc_prim_store();
   }

   /* Reset our structures for the next run of vertices:
    */
   reset_counters(ctx);
}


/**
 * This is called when we fill a vertex buffer before we hit a glEnd().
 * We
 * TODO -- If no new vertices have been stored, don't bother saving it.
 */
static void
wrap_buffers(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   GLint i = save->prim_count - 1;
   GLenum mode;
   GLboolean weak;
   GLboolean no_current_update;

   assert(i < (GLint) save->prim_max);
   assert(i >= 0);

   /* Close off in-progress primitive.
    */
   save->prims[i].count = (save->vert_count - save->prims[i].start);
   mode = save->prims[i].mode;
   weak = save->prims[i].weak;
   no_current_update = save->prims[i].no_current_update;

   /* store the copied vertices, and allocate a new list.
    */
   compile_vertex_list(ctx);

   /* Restart interrupted primitive
    */
   save->prims[0].mode = mode;
   save->prims[0].weak = weak;
   save->prims[0].no_current_update = no_current_update;
   save->prims[0].begin = 0;
   save->prims[0].end = 0;
   save->prims[0].pad = 0;
   save->prims[0].start = 0;
   save->prims[0].count = 0;
   save->prims[0].num_instances = 1;
   save->prims[0].base_instance = 0;
   save->prims[0].is_indirect = 0;
   save->prim_count = 1;
}


/**
 * Called only when buffers are wrapped as the result of filling the
 * vertex_store struct.
 */
static void
wrap_filled_vertex(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   unsigned numComponents;

   /* Emit a glEnd to close off the last vertex list.
    */
   wrap_buffers(ctx);

   /* Copy stored stored vertices to start of new list.
    */
   assert(save->max_vert - save->vert_count > save->copied.nr);

   numComponents = save->copied.nr * save->vertex_size;
   memcpy(save->buffer_ptr,
          save->copied.buffer,
          numComponents * sizeof(fi_type));
   save->buffer_ptr += numComponents;
   save->vert_count += save->copied.nr;
}


static void
copy_to_current(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   GLbitfield64 enabled = save->enabled & (~BITFIELD64_BIT(VBO_ATTRIB_POS));

   while (enabled) {
      const int i = u_bit_scan64(&enabled);
      assert(save->attrsz[i]);

      save->currentsz[i][0] = save->attrsz[i];
      COPY_CLEAN_4V_TYPE_AS_UNION(save->current[i], save->attrsz[i],
                                  save->attrptr[i], save->attrtype[i]);
   }
}


static void
copy_from_current(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   GLbitfield64 enabled = save->enabled & (~BITFIELD64_BIT(VBO_ATTRIB_POS));

   while (enabled) {
      const int i = u_bit_scan64(&enabled);

      switch (save->attrsz[i]) {
      case 4:
         save->attrptr[i][3] = save->current[i][3];
      case 3:
         save->attrptr[i][2] = save->current[i][2];
      case 2:
         save->attrptr[i][1] = save->current[i][1];
      case 1:
         save->attrptr[i][0] = save->current[i][0];
         break;
      case 0:
         unreachable("Unexpected vertex attribute size");
      }
   }
}


/**
 * Called when we increase the size of a vertex attribute.  For example,
 * if we've seen one or more glTexCoord2f() calls and now we get a
 * glTexCoord3f() call.
 * Flush existing data, set new attrib size, replay copied vertices.
 */
static void
upgrade_vertex(struct gl_context *ctx, GLuint attr, GLuint newsz)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   GLuint oldsz;
   GLuint i;
   fi_type *tmp;

   /* Store the current run of vertices, and emit a GL_END.  Emit a
    * BEGIN in the new buffer.
    */
   if (save->vert_count)
      wrap_buffers(ctx);
   else
      assert(save->copied.nr == 0);

   /* Do a COPY_TO_CURRENT to ensure back-copying works for the case
    * when the attribute already exists in the vertex and is having
    * its size increased.
    */
   copy_to_current(ctx);

   /* Fix up sizes:
    */
   oldsz = save->attrsz[attr];
   save->attrsz[attr] = newsz;
   save->enabled |= BITFIELD64_BIT(attr);

   save->vertex_size += newsz - oldsz;
   save->max_vert = ((VBO_SAVE_BUFFER_SIZE - save->vertex_store->used) /
                     save->vertex_size);
   save->vert_count = 0;

   /* Recalculate all the attrptr[] values:
    */
   tmp = save->vertex;
   for (i = 0; i < VBO_ATTRIB_MAX; i++) {
      if (save->attrsz[i]) {
         save->attrptr[i] = tmp;
         tmp += save->attrsz[i];
      }
      else {
         save->attrptr[i] = NULL;       /* will not be dereferenced. */
      }
   }

   /* Copy from current to repopulate the vertex with correct values.
    */
   copy_from_current(ctx);

   /* Replay stored vertices to translate them to new format here.
    *
    * If there are copied vertices and the new (upgraded) attribute
    * has not been defined before, this list is somewhat degenerate,
    * and will need fixup at runtime.
    */
   if (save->copied.nr) {
      const fi_type *data = save->copied.buffer;
      fi_type *dest = save->buffer_map;

      /* Need to note this and fix up at runtime (or loopback):
       */
      if (attr != VBO_ATTRIB_POS && save->currentsz[attr][0] == 0) {
         assert(oldsz == 0);
         save->dangling_attr_ref = GL_TRUE;
      }

      for (i = 0; i < save->copied.nr; i++) {
         GLbitfield64 enabled = save->enabled;
         while (enabled) {
            const int j = u_bit_scan64(&enabled);
            assert(save->attrsz[j]);
            if (j == attr) {
               if (oldsz) {
                  COPY_CLEAN_4V_TYPE_AS_UNION(dest, oldsz, data,
                                              save->attrtype[j]);
                  data += oldsz;
                  dest += newsz;
               }
               else {
                  COPY_SZ_4V(dest, newsz, save->current[attr]);
                  dest += newsz;
               }
            }
            else {
               GLint sz = save->attrsz[j];
               COPY_SZ_4V(dest, sz, data);
               data += sz;
               dest += sz;
            }
         }
      }

      save->buffer_ptr = dest;
      save->vert_count += save->copied.nr;
   }
}


/**
 * This is called when the size of a vertex attribute changes.
 * For example, after seeing one or more glTexCoord2f() calls we
 * get a glTexCoord4f() or glTexCoord1f() call.
 */
static void
fixup_vertex(struct gl_context *ctx, GLuint attr, GLuint sz)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   if (sz > save->attrsz[attr]) {
      /* New size is larger.  Need to flush existing vertices and get
       * an enlarged vertex format.
       */
      upgrade_vertex(ctx, attr, sz);
   }
   else if (sz < save->active_sz[attr]) {
      GLuint i;
      const fi_type *id = vbo_get_default_vals_as_union(save->attrtype[attr]);

      /* New size is equal or smaller - just need to fill in some
       * zeros.
       */
      for (i = sz; i <= save->attrsz[attr]; i++)
         save->attrptr[attr][i - 1] = id[i - 1];
   }

   save->active_sz[attr] = sz;
}


/**
 * Reset the current size of all vertex attributes to the default
 * value of 0.  This signals that we haven't yet seen any per-vertex
 * commands such as glNormal3f() or glTexCoord2f().
 */
static void
reset_vertex(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   while (save->enabled) {
      const int i = u_bit_scan64(&save->enabled);
      assert(save->attrsz[i]);
      save->attrsz[i] = 0;
      save->active_sz[i] = 0;
   }

   save->vertex_size = 0;
}



#define ERROR(err)   _mesa_compile_error(ctx, err, __func__);


/* Only one size for each attribute may be active at once.  Eg. if
 * Color3f is installed/active, then Color4f may not be, even if the
 * vertex actually contains 4 color coordinates.  This is because the
 * 3f version won't otherwise set color[3] to 1.0 -- this is the job
 * of the chooser function when switching between Color4f and Color3f.
 */
#define ATTR_UNION(A, N, T, C, V0, V1, V2, V3)			\
do {								\
   struct vbo_save_context *save = &vbo_context(ctx)->save;	\
								\
   if (save->active_sz[A] != N)					\
      fixup_vertex(ctx, A, N);					\
								\
   {								\
      C *dest = (C *)save->attrptr[A];                          \
      if (N>0) dest[0] = V0;					\
      if (N>1) dest[1] = V1;					\
      if (N>2) dest[2] = V2;					\
      if (N>3) dest[3] = V3;					\
      save->attrtype[A] = T;					\
   }								\
								\
   if ((A) == 0) {						\
      GLuint i;							\
								\
      for (i = 0; i < save->vertex_size; i++)			\
	 save->buffer_ptr[i] = save->vertex[i];			\
								\
      save->buffer_ptr += save->vertex_size;			\
								\
      if (++save->vert_count >= save->max_vert)			\
	 wrap_filled_vertex(ctx);				\
   }								\
} while (0)

#define TAG(x) _save_##x

#include "vbo_attrib_tmp.h"



#define MAT( ATTR, N, face, params )			\
do {							\
   if (face != GL_BACK)					\
      MAT_ATTR( ATTR, N, params ); /* front */		\
   if (face != GL_FRONT)				\
      MAT_ATTR( ATTR + 1, N, params ); /* back */	\
} while (0)


/**
 * Save a glMaterial call found between glBegin/End.
 * glMaterial calls outside Begin/End are handled in dlist.c.
 */
static void GLAPIENTRY
_save_Materialfv(GLenum face, GLenum pname, const GLfloat *params)
{
   GET_CURRENT_CONTEXT(ctx);

   if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glMaterial(face)");
      return;
   }

   switch (pname) {
   case GL_EMISSION:
      MAT(VBO_ATTRIB_MAT_FRONT_EMISSION, 4, face, params);
      break;
   case GL_AMBIENT:
      MAT(VBO_ATTRIB_MAT_FRONT_AMBIENT, 4, face, params);
      break;
   case GL_DIFFUSE:
      MAT(VBO_ATTRIB_MAT_FRONT_DIFFUSE, 4, face, params);
      break;
   case GL_SPECULAR:
      MAT(VBO_ATTRIB_MAT_FRONT_SPECULAR, 4, face, params);
      break;
   case GL_SHININESS:
      if (*params < 0 || *params > ctx->Const.MaxShininess) {
         _mesa_compile_error(ctx, GL_INVALID_VALUE, "glMaterial(shininess)");
      }
      else {
         MAT(VBO_ATTRIB_MAT_FRONT_SHININESS, 1, face, params);
      }
      break;
   case GL_COLOR_INDEXES:
      MAT(VBO_ATTRIB_MAT_FRONT_INDEXES, 3, face, params);
      break;
   case GL_AMBIENT_AND_DIFFUSE:
      MAT(VBO_ATTRIB_MAT_FRONT_AMBIENT, 4, face, params);
      MAT(VBO_ATTRIB_MAT_FRONT_DIFFUSE, 4, face, params);
      break;
   default:
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glMaterial(pname)");
      return;
   }
}


/* Cope with EvalCoord/CallList called within a begin/end object:
 *     -- Flush current buffer
 *     -- Fallback to opcodes for the rest of the begin/end object.
 */
static void
dlist_fallback(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   if (save->vert_count || save->prim_count) {
      if (save->prim_count > 0) {
         /* Close off in-progress primitive. */
         GLint i = save->prim_count - 1;
         save->prims[i].count = save->vert_count - save->prims[i].start;
      }

      /* Need to replay this display list with loopback,
       * unfortunately, otherwise this primitive won't be handled
       * properly:
       */
      save->dangling_attr_ref = GL_TRUE;

      compile_vertex_list(ctx);
   }

   copy_to_current(ctx);
   reset_vertex(ctx);
   reset_counters(ctx);
   if (save->out_of_memory) {
      _mesa_install_save_vtxfmt(ctx, &save->vtxfmt_noop);
   }
   else {
      _mesa_install_save_vtxfmt(ctx, &ctx->ListState.ListVtxfmt);
   }
   ctx->Driver.SaveNeedFlush = GL_FALSE;
}


static void GLAPIENTRY
_save_EvalCoord1f(GLfloat u)
{
   GET_CURRENT_CONTEXT(ctx);
   dlist_fallback(ctx);
   CALL_EvalCoord1f(ctx->Save, (u));
}

static void GLAPIENTRY
_save_EvalCoord1fv(const GLfloat * v)
{
   GET_CURRENT_CONTEXT(ctx);
   dlist_fallback(ctx);
   CALL_EvalCoord1fv(ctx->Save, (v));
}

static void GLAPIENTRY
_save_EvalCoord2f(GLfloat u, GLfloat v)
{
   GET_CURRENT_CONTEXT(ctx);
   dlist_fallback(ctx);
   CALL_EvalCoord2f(ctx->Save, (u, v));
}

static void GLAPIENTRY
_save_EvalCoord2fv(const GLfloat * v)
{
   GET_CURRENT_CONTEXT(ctx);
   dlist_fallback(ctx);
   CALL_EvalCoord2fv(ctx->Save, (v));
}

static void GLAPIENTRY
_save_EvalPoint1(GLint i)
{
   GET_CURRENT_CONTEXT(ctx);
   dlist_fallback(ctx);
   CALL_EvalPoint1(ctx->Save, (i));
}

static void GLAPIENTRY
_save_EvalPoint2(GLint i, GLint j)
{
   GET_CURRENT_CONTEXT(ctx);
   dlist_fallback(ctx);
   CALL_EvalPoint2(ctx->Save, (i, j));
}

static void GLAPIENTRY
_save_CallList(GLuint l)
{
   GET_CURRENT_CONTEXT(ctx);
   dlist_fallback(ctx);
   CALL_CallList(ctx->Save, (l));
}

static void GLAPIENTRY
_save_CallLists(GLsizei n, GLenum type, const GLvoid * v)
{
   GET_CURRENT_CONTEXT(ctx);
   dlist_fallback(ctx);
   CALL_CallLists(ctx->Save, (n, type, v));
}



/**
 * Called when a glBegin is getting compiled into a display list.
 * Updating of ctx->Driver.CurrentSavePrimitive is already taken care of.
 */
void
vbo_save_NotifyBegin(struct gl_context *ctx, GLenum mode)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   const GLuint i = save->prim_count++;

   assert(i < save->prim_max);
   save->prims[i].mode = mode & VBO_SAVE_PRIM_MODE_MASK;
   save->prims[i].begin = 1;
   save->prims[i].end = 0;
   save->prims[i].weak = (mode & VBO_SAVE_PRIM_WEAK) ? 1 : 0;
   save->prims[i].no_current_update =
      (mode & VBO_SAVE_PRIM_NO_CURRENT_UPDATE) ? 1 : 0;
   save->prims[i].pad = 0;
   save->prims[i].start = save->vert_count;
   save->prims[i].count = 0;
   save->prims[i].num_instances = 1;
   save->prims[i].base_instance = 0;
   save->prims[i].is_indirect = 0;

   if (save->out_of_memory) {
      _mesa_install_save_vtxfmt(ctx, &save->vtxfmt_noop);
   }
   else {
      _mesa_install_save_vtxfmt(ctx, &save->vtxfmt);
   }

   /* We need to call vbo_save_SaveFlushVertices() if there's state change */
   ctx->Driver.SaveNeedFlush = GL_TRUE;
}


static void GLAPIENTRY
_save_End(void)
{
   GET_CURRENT_CONTEXT(ctx);
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   const GLint i = save->prim_count - 1;

   ctx->Driver.CurrentSavePrimitive = PRIM_OUTSIDE_BEGIN_END;
   save->prims[i].end = 1;
   save->prims[i].count = (save->vert_count - save->prims[i].start);

   if (i == (GLint) save->prim_max - 1) {
      compile_vertex_list(ctx);
      assert(save->copied.nr == 0);
   }

   /* Swap out this vertex format while outside begin/end.  Any color,
    * etc. received between here and the next begin will be compiled
    * as opcodes.
    */
   if (save->out_of_memory) {
      _mesa_install_save_vtxfmt(ctx, &save->vtxfmt_noop);
   }
   else {
      _mesa_install_save_vtxfmt(ctx, &ctx->ListState.ListVtxfmt);
   }
}


static void GLAPIENTRY
_save_Begin(GLenum mode)
{
   GET_CURRENT_CONTEXT(ctx);
   (void) mode;
   _mesa_compile_error(ctx, GL_INVALID_OPERATION, "Recursive glBegin");
}


static void GLAPIENTRY
_save_PrimitiveRestartNV(void)
{
   GET_CURRENT_CONTEXT(ctx);
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   if (save->prim_count == 0) {
      /* We're not inside a glBegin/End pair, so calling glPrimitiverRestartNV
       * is an error.
       */
      _mesa_compile_error(ctx, GL_INVALID_OPERATION,
                          "glPrimitiveRestartNV called outside glBegin/End");
   } else {
      /* get current primitive mode */
      GLenum curPrim = save->prims[save->prim_count - 1].mode;

      /* restart primitive */
      CALL_End(GET_DISPATCH(), ());
      vbo_save_NotifyBegin(ctx, curPrim);
   }
}


/* Unlike the functions above, these are to be hooked into the vtxfmt
 * maintained in ctx->ListState, active when the list is known or
 * suspected to be outside any begin/end primitive.
 * Note: OBE = Outside Begin/End
 */
static void GLAPIENTRY
_save_OBE_Rectf(GLfloat x1, GLfloat y1, GLfloat x2, GLfloat y2)
{
   GET_CURRENT_CONTEXT(ctx);
   vbo_save_NotifyBegin(ctx, GL_QUADS | VBO_SAVE_PRIM_WEAK);
   CALL_Vertex2f(GET_DISPATCH(), (x1, y1));
   CALL_Vertex2f(GET_DISPATCH(), (x2, y1));
   CALL_Vertex2f(GET_DISPATCH(), (x2, y2));
   CALL_Vertex2f(GET_DISPATCH(), (x1, y2));
   CALL_End(GET_DISPATCH(), ());
}


static void GLAPIENTRY
_save_OBE_DrawArrays(GLenum mode, GLint start, GLsizei count)
{
   GET_CURRENT_CONTEXT(ctx);
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   GLint i;

   if (!_mesa_is_valid_prim_mode(ctx, mode)) {
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glDrawArrays(mode)");
      return;
   }
   if (count < 0) {
      _mesa_compile_error(ctx, GL_INVALID_VALUE, "glDrawArrays(count<0)");
      return;
   }

   if (save->out_of_memory)
      return;

   /* Make sure to process any VBO binding changes */
   _mesa_update_state(ctx);

   _ae_map_vbos(ctx);

   vbo_save_NotifyBegin(ctx, (mode | VBO_SAVE_PRIM_WEAK
                              | VBO_SAVE_PRIM_NO_CURRENT_UPDATE));

   for (i = 0; i < count; i++)
      CALL_ArrayElement(GET_DISPATCH(), (start + i));
   CALL_End(GET_DISPATCH(), ());

   _ae_unmap_vbos(ctx);
}


static void GLAPIENTRY
_save_OBE_MultiDrawArrays(GLenum mode, const GLint *first,
                          const GLsizei *count, GLsizei primcount)
{
   GET_CURRENT_CONTEXT(ctx);
   GLint i;

   if (!_mesa_is_valid_prim_mode(ctx, mode)) {
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glMultiDrawArrays(mode)");
      return;
   }

   if (primcount < 0) {
      _mesa_compile_error(ctx, GL_INVALID_VALUE,
                          "glMultiDrawArrays(primcount<0)");
      return;
   }

   for (i = 0; i < primcount; i++) {
      if (count[i] < 0) {
         _mesa_compile_error(ctx, GL_INVALID_VALUE,
                             "glMultiDrawArrays(count[i]<0)");
         return;
      }
   }

   for (i = 0; i < primcount; i++) {
      if (count[i] > 0) {
         _save_OBE_DrawArrays(mode, first[i], count[i]);
      }
   }
}


/* Could do better by copying the arrays and element list intact and
 * then emitting an indexed prim at runtime.
 */
static void GLAPIENTRY
_save_OBE_DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type,
                                 const GLvoid * indices, GLint basevertex)
{
   GET_CURRENT_CONTEXT(ctx);
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   struct gl_buffer_object *indexbuf = ctx->Array.VAO->IndexBufferObj;
   GLint i;

   if (!_mesa_is_valid_prim_mode(ctx, mode)) {
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glDrawElements(mode)");
      return;
   }
   if (count < 0) {
      _mesa_compile_error(ctx, GL_INVALID_VALUE, "glDrawElements(count<0)");
      return;
   }
   if (type != GL_UNSIGNED_BYTE &&
       type != GL_UNSIGNED_SHORT &&
       type != GL_UNSIGNED_INT) {
      _mesa_compile_error(ctx, GL_INVALID_VALUE, "glDrawElements(count<0)");
      return;
   }

   if (save->out_of_memory)
      return;

   /* Make sure to process any VBO binding changes */
   _mesa_update_state(ctx);

   _ae_map_vbos(ctx);

   if (_mesa_is_bufferobj(indexbuf))
      indices =
         ADD_POINTERS(indexbuf->Mappings[MAP_INTERNAL].Pointer, indices);

   vbo_save_NotifyBegin(ctx, (mode | VBO_SAVE_PRIM_WEAK |
                              VBO_SAVE_PRIM_NO_CURRENT_UPDATE));

   switch (type) {
   case GL_UNSIGNED_BYTE:
      for (i = 0; i < count; i++)
         CALL_ArrayElement(GET_DISPATCH(), (basevertex + ((GLubyte *) indices)[i]));
      break;
   case GL_UNSIGNED_SHORT:
      for (i = 0; i < count; i++)
         CALL_ArrayElement(GET_DISPATCH(), (basevertex + ((GLushort *) indices)[i]));
      break;
   case GL_UNSIGNED_INT:
      for (i = 0; i < count; i++)
         CALL_ArrayElement(GET_DISPATCH(), (basevertex + ((GLuint *) indices)[i]));
      break;
   default:
      _mesa_error(ctx, GL_INVALID_ENUM, "glDrawElements(type)");
      break;
   }

   CALL_End(GET_DISPATCH(), ());

   _ae_unmap_vbos(ctx);
}

static void GLAPIENTRY
_save_OBE_DrawElements(GLenum mode, GLsizei count, GLenum type,
                       const GLvoid * indices)
{
   _save_OBE_DrawElementsBaseVertex(mode, count, type, indices, 0);
}


static void GLAPIENTRY
_save_OBE_DrawRangeElements(GLenum mode, GLuint start, GLuint end,
                            GLsizei count, GLenum type,
                            const GLvoid * indices)
{
   GET_CURRENT_CONTEXT(ctx);
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   if (!_mesa_is_valid_prim_mode(ctx, mode)) {
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glDrawRangeElements(mode)");
      return;
   }
   if (count < 0) {
      _mesa_compile_error(ctx, GL_INVALID_VALUE,
                          "glDrawRangeElements(count<0)");
      return;
   }
   if (type != GL_UNSIGNED_BYTE &&
       type != GL_UNSIGNED_SHORT &&
       type != GL_UNSIGNED_INT) {
      _mesa_compile_error(ctx, GL_INVALID_ENUM, "glDrawRangeElements(type)");
      return;
   }
   if (end < start) {
      _mesa_compile_error(ctx, GL_INVALID_VALUE,
                          "glDrawRangeElements(end < start)");
      return;
   }

   if (save->out_of_memory)
      return;

   _save_OBE_DrawElements(mode, count, type, indices);
}


static void GLAPIENTRY
_save_OBE_MultiDrawElements(GLenum mode, const GLsizei *count, GLenum type,
                            const GLvoid * const *indices, GLsizei primcount)
{
   GLsizei i;

   for (i = 0; i < primcount; i++) {
      if (count[i] > 0) {
	 CALL_DrawElements(GET_DISPATCH(), (mode, count[i], type, indices[i]));
      }
   }
}


static void GLAPIENTRY
_save_OBE_MultiDrawElementsBaseVertex(GLenum mode, const GLsizei *count,
                                      GLenum type,
                                      const GLvoid * const *indices,
                                      GLsizei primcount,
                                      const GLint *basevertex)
{
   GLsizei i;

   for (i = 0; i < primcount; i++) {
      if (count[i] > 0) {
	 CALL_DrawElementsBaseVertex(GET_DISPATCH(), (mode, count[i], type,
						      indices[i],
						      basevertex[i]));
      }
   }
}


static void
vtxfmt_init(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   GLvertexformat *vfmt = &save->vtxfmt;

   vfmt->ArrayElement = _ae_ArrayElement;

   vfmt->Color3f = _save_Color3f;
   vfmt->Color3fv = _save_Color3fv;
   vfmt->Color4f = _save_Color4f;
   vfmt->Color4fv = _save_Color4fv;
   vfmt->EdgeFlag = _save_EdgeFlag;
   vfmt->End = _save_End;
   vfmt->PrimitiveRestartNV = _save_PrimitiveRestartNV;
   vfmt->FogCoordfEXT = _save_FogCoordfEXT;
   vfmt->FogCoordfvEXT = _save_FogCoordfvEXT;
   vfmt->Indexf = _save_Indexf;
   vfmt->Indexfv = _save_Indexfv;
   vfmt->Materialfv = _save_Materialfv;
   vfmt->MultiTexCoord1fARB = _save_MultiTexCoord1f;
   vfmt->MultiTexCoord1fvARB = _save_MultiTexCoord1fv;
   vfmt->MultiTexCoord2fARB = _save_MultiTexCoord2f;
   vfmt->MultiTexCoord2fvARB = _save_MultiTexCoord2fv;
   vfmt->MultiTexCoord3fARB = _save_MultiTexCoord3f;
   vfmt->MultiTexCoord3fvARB = _save_MultiTexCoord3fv;
   vfmt->MultiTexCoord4fARB = _save_MultiTexCoord4f;
   vfmt->MultiTexCoord4fvARB = _save_MultiTexCoord4fv;
   vfmt->Normal3f = _save_Normal3f;
   vfmt->Normal3fv = _save_Normal3fv;
   vfmt->SecondaryColor3fEXT = _save_SecondaryColor3fEXT;
   vfmt->SecondaryColor3fvEXT = _save_SecondaryColor3fvEXT;
   vfmt->TexCoord1f = _save_TexCoord1f;
   vfmt->TexCoord1fv = _save_TexCoord1fv;
   vfmt->TexCoord2f = _save_TexCoord2f;
   vfmt->TexCoord2fv = _save_TexCoord2fv;
   vfmt->TexCoord3f = _save_TexCoord3f;
   vfmt->TexCoord3fv = _save_TexCoord3fv;
   vfmt->TexCoord4f = _save_TexCoord4f;
   vfmt->TexCoord4fv = _save_TexCoord4fv;
   vfmt->Vertex2f = _save_Vertex2f;
   vfmt->Vertex2fv = _save_Vertex2fv;
   vfmt->Vertex3f = _save_Vertex3f;
   vfmt->Vertex3fv = _save_Vertex3fv;
   vfmt->Vertex4f = _save_Vertex4f;
   vfmt->Vertex4fv = _save_Vertex4fv;
   vfmt->VertexAttrib1fARB = _save_VertexAttrib1fARB;
   vfmt->VertexAttrib1fvARB = _save_VertexAttrib1fvARB;
   vfmt->VertexAttrib2fARB = _save_VertexAttrib2fARB;
   vfmt->VertexAttrib2fvARB = _save_VertexAttrib2fvARB;
   vfmt->VertexAttrib3fARB = _save_VertexAttrib3fARB;
   vfmt->VertexAttrib3fvARB = _save_VertexAttrib3fvARB;
   vfmt->VertexAttrib4fARB = _save_VertexAttrib4fARB;
   vfmt->VertexAttrib4fvARB = _save_VertexAttrib4fvARB;

   vfmt->VertexAttrib1fNV = _save_VertexAttrib1fNV;
   vfmt->VertexAttrib1fvNV = _save_VertexAttrib1fvNV;
   vfmt->VertexAttrib2fNV = _save_VertexAttrib2fNV;
   vfmt->VertexAttrib2fvNV = _save_VertexAttrib2fvNV;
   vfmt->VertexAttrib3fNV = _save_VertexAttrib3fNV;
   vfmt->VertexAttrib3fvNV = _save_VertexAttrib3fvNV;
   vfmt->VertexAttrib4fNV = _save_VertexAttrib4fNV;
   vfmt->VertexAttrib4fvNV = _save_VertexAttrib4fvNV;

   /* integer-valued */
   vfmt->VertexAttribI1i = _save_VertexAttribI1i;
   vfmt->VertexAttribI2i = _save_VertexAttribI2i;
   vfmt->VertexAttribI3i = _save_VertexAttribI3i;
   vfmt->VertexAttribI4i = _save_VertexAttribI4i;
   vfmt->VertexAttribI2iv = _save_VertexAttribI2iv;
   vfmt->VertexAttribI3iv = _save_VertexAttribI3iv;
   vfmt->VertexAttribI4iv = _save_VertexAttribI4iv;

   /* unsigned integer-valued */
   vfmt->VertexAttribI1ui = _save_VertexAttribI1ui;
   vfmt->VertexAttribI2ui = _save_VertexAttribI2ui;
   vfmt->VertexAttribI3ui = _save_VertexAttribI3ui;
   vfmt->VertexAttribI4ui = _save_VertexAttribI4ui;
   vfmt->VertexAttribI2uiv = _save_VertexAttribI2uiv;
   vfmt->VertexAttribI3uiv = _save_VertexAttribI3uiv;
   vfmt->VertexAttribI4uiv = _save_VertexAttribI4uiv;

   vfmt->VertexP2ui = _save_VertexP2ui;
   vfmt->VertexP3ui = _save_VertexP3ui;
   vfmt->VertexP4ui = _save_VertexP4ui;
   vfmt->VertexP2uiv = _save_VertexP2uiv;
   vfmt->VertexP3uiv = _save_VertexP3uiv;
   vfmt->VertexP4uiv = _save_VertexP4uiv;

   vfmt->TexCoordP1ui = _save_TexCoordP1ui;
   vfmt->TexCoordP2ui = _save_TexCoordP2ui;
   vfmt->TexCoordP3ui = _save_TexCoordP3ui;
   vfmt->TexCoordP4ui = _save_TexCoordP4ui;
   vfmt->TexCoordP1uiv = _save_TexCoordP1uiv;
   vfmt->TexCoordP2uiv = _save_TexCoordP2uiv;
   vfmt->TexCoordP3uiv = _save_TexCoordP3uiv;
   vfmt->TexCoordP4uiv = _save_TexCoordP4uiv;

   vfmt->MultiTexCoordP1ui = _save_MultiTexCoordP1ui;
   vfmt->MultiTexCoordP2ui = _save_MultiTexCoordP2ui;
   vfmt->MultiTexCoordP3ui = _save_MultiTexCoordP3ui;
   vfmt->MultiTexCoordP4ui = _save_MultiTexCoordP4ui;
   vfmt->MultiTexCoordP1uiv = _save_MultiTexCoordP1uiv;
   vfmt->MultiTexCoordP2uiv = _save_MultiTexCoordP2uiv;
   vfmt->MultiTexCoordP3uiv = _save_MultiTexCoordP3uiv;
   vfmt->MultiTexCoordP4uiv = _save_MultiTexCoordP4uiv;

   vfmt->NormalP3ui = _save_NormalP3ui;
   vfmt->NormalP3uiv = _save_NormalP3uiv;

   vfmt->ColorP3ui = _save_ColorP3ui;
   vfmt->ColorP4ui = _save_ColorP4ui;
   vfmt->ColorP3uiv = _save_ColorP3uiv;
   vfmt->ColorP4uiv = _save_ColorP4uiv;

   vfmt->SecondaryColorP3ui = _save_SecondaryColorP3ui;
   vfmt->SecondaryColorP3uiv = _save_SecondaryColorP3uiv;

   vfmt->VertexAttribP1ui = _save_VertexAttribP1ui;
   vfmt->VertexAttribP2ui = _save_VertexAttribP2ui;
   vfmt->VertexAttribP3ui = _save_VertexAttribP3ui;
   vfmt->VertexAttribP4ui = _save_VertexAttribP4ui;

   vfmt->VertexAttribP1uiv = _save_VertexAttribP1uiv;
   vfmt->VertexAttribP2uiv = _save_VertexAttribP2uiv;
   vfmt->VertexAttribP3uiv = _save_VertexAttribP3uiv;
   vfmt->VertexAttribP4uiv = _save_VertexAttribP4uiv;

   vfmt->VertexAttribL1d = _save_VertexAttribL1d;
   vfmt->VertexAttribL2d = _save_VertexAttribL2d;
   vfmt->VertexAttribL3d = _save_VertexAttribL3d;
   vfmt->VertexAttribL4d = _save_VertexAttribL4d;

   vfmt->VertexAttribL1dv = _save_VertexAttribL1dv;
   vfmt->VertexAttribL2dv = _save_VertexAttribL2dv;
   vfmt->VertexAttribL3dv = _save_VertexAttribL3dv;
   vfmt->VertexAttribL4dv = _save_VertexAttribL4dv;

   vfmt->VertexAttribL1ui64ARB = _save_VertexAttribL1ui64ARB;
   vfmt->VertexAttribL1ui64vARB = _save_VertexAttribL1ui64vARB;

   /* This will all require us to fallback to saving the list as opcodes:
    */
   vfmt->CallList = _save_CallList;
   vfmt->CallLists = _save_CallLists;

   vfmt->EvalCoord1f = _save_EvalCoord1f;
   vfmt->EvalCoord1fv = _save_EvalCoord1fv;
   vfmt->EvalCoord2f = _save_EvalCoord2f;
   vfmt->EvalCoord2fv = _save_EvalCoord2fv;
   vfmt->EvalPoint1 = _save_EvalPoint1;
   vfmt->EvalPoint2 = _save_EvalPoint2;

   /* These calls all generate GL_INVALID_OPERATION since this vtxfmt is
    * only used when we're inside a glBegin/End pair.
    */
   vfmt->Begin = _save_Begin;
}


/**
 * Initialize the dispatch table with the VBO functions for display
 * list compilation.
 */
void
vbo_initialize_save_dispatch(const struct gl_context *ctx,
                             struct _glapi_table *exec)
{
   SET_DrawArrays(exec, _save_OBE_DrawArrays);
   SET_MultiDrawArrays(exec, _save_OBE_MultiDrawArrays);
   SET_DrawElements(exec, _save_OBE_DrawElements);
   SET_DrawElementsBaseVertex(exec, _save_OBE_DrawElementsBaseVertex);
   SET_DrawRangeElements(exec, _save_OBE_DrawRangeElements);
   SET_MultiDrawElementsEXT(exec, _save_OBE_MultiDrawElements);
   SET_MultiDrawElementsBaseVertex(exec, _save_OBE_MultiDrawElementsBaseVertex);
   SET_Rectf(exec, _save_OBE_Rectf);
   /* Note: other glDraw functins aren't compiled into display lists */
}



void
vbo_save_SaveFlushVertices(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   /* Noop when we are actually active:
    */
   if (ctx->Driver.CurrentSavePrimitive <= PRIM_MAX)
      return;

   if (save->vert_count || save->prim_count)
      compile_vertex_list(ctx);

   copy_to_current(ctx);
   reset_vertex(ctx);
   reset_counters(ctx);
   ctx->Driver.SaveNeedFlush = GL_FALSE;
}


/**
 * Called from glNewList when we're starting to compile a display list.
 */
void
vbo_save_NewList(struct gl_context *ctx, GLuint list, GLenum mode)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   (void) list;
   (void) mode;

   if (!save->prim_store)
      save->prim_store = alloc_prim_store();

   if (!save->vertex_store)
      save->vertex_store = alloc_vertex_store(ctx);

   save->buffer_ptr = vbo_save_map_vertex_store(ctx, save->vertex_store);

   reset_vertex(ctx);
   reset_counters(ctx);
   ctx->Driver.SaveNeedFlush = GL_FALSE;
}


/**
 * Called from glEndList when we're finished compiling a display list.
 */
void
vbo_save_EndList(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   /* EndList called inside a (saved) Begin/End pair?
    */
   if (_mesa_inside_dlist_begin_end(ctx)) {
      if (save->prim_count > 0) {
         GLint i = save->prim_count - 1;
         ctx->Driver.CurrentSavePrimitive = PRIM_OUTSIDE_BEGIN_END;
         save->prims[i].end = 0;
         save->prims[i].count = save->vert_count - save->prims[i].start;
      }

      /* Make sure this vertex list gets replayed by the "loopback"
       * mechanism:
       */
      save->dangling_attr_ref = GL_TRUE;
      vbo_save_SaveFlushVertices(ctx);

      /* Swap out this vertex format while outside begin/end.  Any color,
       * etc. received between here and the next begin will be compiled
       * as opcodes.
       */
      _mesa_install_save_vtxfmt(ctx, &ctx->ListState.ListVtxfmt);
   }

   vbo_save_unmap_vertex_store(ctx, save->vertex_store);

   assert(save->vertex_size == 0);
}


/**
 * Called from the display list code when we're about to execute a
 * display list.
 */
void
vbo_save_BeginCallList(struct gl_context *ctx, struct gl_display_list *dlist)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   save->replay_flags |= dlist->Flags;
}


/**
 * Called from the display list code when we're finished executing a
 * display list.
 */
void
vbo_save_EndCallList(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;

   if (ctx->ListState.CallDepth == 1) {
      /* This is correct: want to keep only the VBO_SAVE_FALLBACK
       * flag, if it is set:
       */
      save->replay_flags &= VBO_SAVE_FALLBACK;
   }
}


/**
 * Called by display list code when a display list is being deleted.
 */
static void
vbo_destroy_vertex_list(struct gl_context *ctx, void *data)
{
   struct vbo_save_vertex_list *node = (struct vbo_save_vertex_list *) data;

   for (gl_vertex_processing_mode vpm = VP_MODE_FF; vpm < VP_MODE_MAX; ++vpm)
      _mesa_reference_vao(ctx, &node->VAO[vpm], NULL);

   if (--node->prim_store->refcount == 0)
      free(node->prim_store);

   free(node->current_data);
   node->current_data = NULL;
}


static void
vbo_print_vertex_list(struct gl_context *ctx, void *data, FILE *f)
{
   struct vbo_save_vertex_list *node = (struct vbo_save_vertex_list *) data;
   GLuint i;
   struct gl_buffer_object *buffer = node->VAO[0]->BufferBinding[0].BufferObj;
   const GLuint vertex_size = _vbo_save_get_stride(node)/sizeof(GLfloat);
   (void) ctx;

   fprintf(f, "VBO-VERTEX-LIST, %u vertices, %d primitives, %d vertsize, "
           "buffer %p\n",
           node->vertex_count, node->prim_count, vertex_size,
           buffer);

   for (i = 0; i < node->prim_count; i++) {
      struct _mesa_prim *prim = &node->prims[i];
      fprintf(f, "   prim %d: %s%s %d..%d %s %s\n",
             i,
             _mesa_lookup_prim_by_nr(prim->mode),
             prim->weak ? " (weak)" : "",
             prim->start,
             prim->start + prim->count,
             (prim->begin) ? "BEGIN" : "(wrap)",
             (prim->end) ? "END" : "(wrap)");
   }
}


/**
 * Called during context creation/init.
 */
static void
current_init(struct gl_context *ctx)
{
   struct vbo_save_context *save = &vbo_context(ctx)->save;
   GLint i;

   for (i = VBO_ATTRIB_POS; i <= VBO_ATTRIB_GENERIC15; i++) {
      const GLuint j = i - VBO_ATTRIB_POS;
      assert(j < VERT_ATTRIB_MAX);
      save->currentsz[i] = &ctx->ListState.ActiveAttribSize[j];
      save->current[i] = (fi_type *) ctx->ListState.CurrentAttrib[j];
   }

   for (i = VBO_ATTRIB_FIRST_MATERIAL; i <= VBO_ATTRIB_LAST_MATERIAL; i++) {
      const GLuint j = i - VBO_ATTRIB_FIRST_MATERIAL;
      assert(j < MAT_ATTRIB_MAX);
      save->currentsz[i] = &ctx->ListState.ActiveMaterialSize[j];
      save->current[i] = (fi_type *) ctx->ListState.CurrentMaterial[j];
   }
}


/**
 * Initialize the display list compiler.  Called during context creation.
 */
void
vbo_save_api_init(struct vbo_save_context *save)
{
   struct gl_context *ctx = save->ctx;

   save->opcode_vertex_list =
      _mesa_dlist_alloc_opcode(ctx,
                               sizeof(struct vbo_save_vertex_list),
                               vbo_save_playback_vertex_list,
                               vbo_destroy_vertex_list,
                               vbo_print_vertex_list);

   vtxfmt_init(ctx);
   current_init(ctx);
   _mesa_noop_vtxfmt_init(&save->vtxfmt_noop);
}
