// ======================================================================== //
// Copyright 2009-2015 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#pragma once

#include "../../common/ray.h"
#include "../../common/scene_subdiv_mesh.h"
#include "filter.h"
#include "../bvh4/bvh4.h"
#include "../../common/subdiv/tessellation.h"
#include "../../common/subdiv/tessellation_cache.h"
#include "subdivpatch1cached.h"
#include "grid_soa.h"

#define FORCE_TRIANGLE_UV 0                //!< returns u,v based on individual triangles instead relative to original patch 

#if FORCE_TRIANGLE_UV
#  pragma message("WARNING: FORCE_TRIANGLE_UV is enabled")
#endif

namespace embree
{
  namespace isa
  {
    class GridSOAIntersector1
    {
    public:
      typedef SubdivPatch1Cached Primitive;
      
      /*! Precalculations for subdiv patch intersection */
      class Precalculations 
      { 
      public:
        __forceinline Precalculations (Ray& ray, const void *ptr) 
          : patch(nullptr) {}

        __forceinline ~Precalculations() 
        {
	  if (patch)
            SharedLazyTessellationCache::sharedLazyTessellationCache.unlock();
        }

      public:
        SubdivPatch1Cached* patch;
      };

      static __forceinline const Vec3<float4> getV012(const float *const grid, const size_t offset0, const size_t offset1)
      {
        const float4 r0 = loadu4f(grid + offset0); 
        const float4 r1 = loadu4f(grid + offset1); // FIXME: this accesses 1 element too much
        return Vec3<float4>(unpacklo(r0,r1),       // r00, r10, r01, r11  
                            shuffle<1,1,2,2>(r0),  // r01, r01, r02, r02
                            shuffle<0,1,1,2>(r1)); // r10, r11, r11, r12
      }

      static __forceinline Vec2<float4> decodeUV(const float4 &uv)
      {
	const int4 i_uv = cast(uv);
	const int4 i_u  = i_uv & 0xffff;
	const int4 i_v  = srl(i_uv,16);
	const float4 u    = (float4)i_u * float4(1.0f/0xFFFF);
	const float4 v    = (float4)i_v * float4(1.0f/0xFFFF);
	return Vec2<float4>(u,v);
      }

      static __forceinline void intersect1_precise_2x3(Ray& ray,
						       const float *const grid_x,
						       const float *const grid_y,
						       const float *const grid_z,
						       const float *const grid_uv,
 						       const size_t line_offset,
 						       Precalculations &pre,
                                                       Scene* scene)
      {
	const size_t offset0 = 0 * line_offset;
	const size_t offset1 = 1 * line_offset;

	const Vec3<float4> tri012_x = getV012(grid_x,offset0,offset1);
	const Vec3<float4> tri012_y = getV012(grid_y,offset0,offset1);
	const Vec3<float4> tri012_z = getV012(grid_z,offset0,offset1);

	const Vec3<float4> v0_org(tri012_x[0],tri012_y[0],tri012_z[0]);
	const Vec3<float4> v1_org(tri012_x[1],tri012_y[1],tri012_z[1]);
	const Vec3<float4> v2_org(tri012_x[2],tri012_y[2],tri012_z[2]);
        
	const Vec3<float4> O = ray.org;
	const Vec3<float4> D = ray.dir;
        
	const Vec3<float4> v0 = v0_org - O;
	const Vec3<float4> v1 = v1_org - O;
	const Vec3<float4> v2 = v2_org - O;
        
	const Vec3<float4> e0 = v2 - v0;
	const Vec3<float4> e1 = v0 - v1;	     
	const Vec3<float4> e2 = v1 - v2;	     
        
	/* calculate geometry normal and denominator */
	const Vec3<float4> Ng1 = cross(e1,e0);
	const Vec3<float4> Ng = Ng1+Ng1;
	const float4 den = dot(Ng,D);
	const float4 absDen = abs(den);
	const float4 sgnDen = signmsk(den);
        
	bool4 valid ( true );
	/* perform edge tests */
	const float4 U = dot(Vec3<float4>(cross(v2+v0,e0)),D) ^ sgnDen;
	valid &= U >= 0.0f;
	if (likely(none(valid))) return;
	const float4 V = dot(Vec3<float4>(cross(v0+v1,e1)),D) ^ sgnDen;
	valid &= V >= 0.0f;
	if (likely(none(valid))) return;
	const float4 W = dot(Vec3<float4>(cross(v1+v2,e2)),D) ^ sgnDen;

	valid &= W >= 0.0f;
	if (likely(none(valid))) return;
        
	/* perform depth test */
	const float4 T = dot(v0,Ng) ^ sgnDen;
	valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);
	if (unlikely(none(valid))) return;
        
	/* perform backface culling */
#if defined(RTCORE_BACKFACE_CULLING) // FIXME: wrong because of flipped normals
	valid &= den > float4(zero);
	if (unlikely(none(valid))) return;
#else
	valid &= den != float4(zero);
	if (unlikely(none(valid))) return;
#endif

        /* ray mask test */
        const size_t geomID = pre.patch->geom;
        const size_t primID = pre.patch->prim;
        Geometry* geometry = scene->get(geomID);
#if defined(RTCORE_RAY_MASK)
        if ((geometry->mask & ray.mask) == 0) return;
#endif
        
	/* calculate hit information */
	const float4 rcpAbsDen = rcp(absDen);
        float4 u =  U*rcpAbsDen;
	float4 v =  V*rcpAbsDen;
	const float4 t = T*rcpAbsDen;
        
#if !FORCE_TRIANGLE_UV
	const Vec3<float4> tri012_uv = getV012(grid_uv,offset0,offset1);	
	const Vec2<float4> uv0 = decodeUV(tri012_uv[0]);
	const Vec2<float4> uv1 = decodeUV(tri012_uv[1]);
	const Vec2<float4> uv2 = decodeUV(tri012_uv[2]);        
	const Vec2<float4> uv = u * uv1 + v * uv2 + (1.0f-u-v) * uv0;        
	u = uv[0];
	v = uv[1];        
#endif
	size_t i = select_min(valid,t);

        /* intersection filter test */
#if defined(RTCORE_INTERSECTION_FILTER)
        if (unlikely(geometry->hasIntersectionFilter1())) 
        {
          while (true) 
          {
            /* call intersection filter function */
            Vec3fa Ng_i = Vec3fa(Ng.x[i],Ng.y[i],Ng.z[i]);
            if (runIntersectionFilter1(geometry,ray,u[i],v[i],t[i],Ng_i,geomID,primID)) {
              return;
            }
            valid[i] = 0;
            if (unlikely(none(valid))) return;
            i = select_min(valid,t);
          }
          return;
        }
#endif

	/* update hit information */
	ray.u         = u[i];
	ray.v         = v[i];
	ray.tfar      = t[i];
        ray.geomID    = pre.patch->geom;
        ray.primID    = pre.patch->prim;
        ray.Ng.x      = Ng.x[i];
        ray.Ng.y      = Ng.y[i];
        ray.Ng.z      = Ng.z[i];
      };

      static __forceinline bool occluded1_precise_2x3(Ray& ray,
						      const float *const grid_x,
						      const float *const grid_y,
						      const float *const grid_z,
						      const float *const grid_uv,
						      const size_t line_offset,
                                                      Precalculations &pre,
                                                      Scene* scene)
      {
	const size_t offset0 = 0 * line_offset;
	const size_t offset1 = 1 * line_offset;

	const Vec3<float4> tri012_x = getV012(grid_x,offset0,offset1);
	const Vec3<float4> tri012_y = getV012(grid_y,offset0,offset1);
	const Vec3<float4> tri012_z = getV012(grid_z,offset0,offset1);

	const Vec3<float4> v0_org(tri012_x[0],tri012_y[0],tri012_z[0]);
	const Vec3<float4> v1_org(tri012_x[1],tri012_y[1],tri012_z[1]);
	const Vec3<float4> v2_org(tri012_x[2],tri012_y[2],tri012_z[2]);
        
        const Vec3<float4> O = ray.org;
        const Vec3<float4> D = ray.dir;
        
        const Vec3<float4> v0 = v0_org - O;
        const Vec3<float4> v1 = v1_org - O;
        const Vec3<float4> v2 = v2_org - O;
        
        const Vec3<float4> e0 = v2 - v0;
        const Vec3<float4> e1 = v0 - v1;	     
        const Vec3<float4> e2 = v1 - v2;	     
        
        /* calculate geometry normal and denominator */
        const Vec3<float4> Ng1 = cross(e1,e0);
        const Vec3<float4> Ng = Ng1+Ng1;
        const float4 den = dot(Ng,D);
        const float4 absDen = abs(den);
        const float4 sgnDen = signmsk(den);
        
        bool4 valid ( true );
        /* perform edge tests */
        const float4 U = dot(Vec3<float4>(cross(v2+v0,e0)),D) ^ sgnDen;
        valid &= U >= 0.0f;
        if (likely(none(valid))) return false;
        const float4 V = dot(Vec3<float4>(cross(v0+v1,e1)),D) ^ sgnDen;
        valid &= V >= 0.0f;
        if (likely(none(valid))) return false;
        const float4 W = dot(Vec3<float4>(cross(v1+v2,e2)),D) ^ sgnDen;
        valid &= W >= 0.0f;
        if (likely(none(valid))) return false;
        
        /* perform depth test */
        const float4 T = dot(v0,Ng) ^ sgnDen;
        valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);
        if (unlikely(none(valid))) return false;
        
        /* perform backface culling */
#if defined(RTCORE_BACKFACE_CULLING) // FIXME: wrong because of flipped normals
        valid &= den > float4(zero);
        if (unlikely(none(valid))) return false;
#else
        valid &= den != float4(zero);
        if (unlikely(none(valid))) return false;
#endif

        /* ray mask test */
        const size_t geomID = pre.patch->geom;
        const size_t primID = pre.patch->prim;
        Geometry* geometry = scene->get(geomID);
#if defined(RTCORE_RAY_MASK)
        if ((geometry->mask & ray.mask) == 0) return false;
#endif
        
        /* intersection filter test */
#if defined(RTCORE_INTERSECTION_FILTER)
        if (unlikely(geometry->hasOcclusionFilter1())) 
        {
          /* calculate hit information */
          const float4 rcpAbsDen = rcp(absDen);
          float4 u =  U*rcpAbsDen;
          float4 v =  V*rcpAbsDen;
          const float4 t = T*rcpAbsDen;
        
#if !FORCE_TRIANGLE_UV
          const Vec3<float4> tri012_uv = getV012(grid_uv,offset0,offset1);	
          const Vec2<float4> uv0 = decodeUV(tri012_uv[0]);
          const Vec2<float4> uv1 = decodeUV(tri012_uv[1]);
          const Vec2<float4> uv2 = decodeUV(tri012_uv[2]);        
          const Vec2<float4> uv = u * uv1 + v * uv2 + (1.0f-u-v) * uv0;        
          u = uv[0];
          v = uv[1];        
#endif
          
          for (size_t m=movemask(valid), i=__bsf(m); m!=0; m=__btc(m,i), i=__bsf(m)) {  
            const Vec3fa Ng_i = Vec3fa(Ng.x[i],Ng.y[i],Ng.z[i]);
            if (runOcclusionFilter1(geometry,ray,u[i],v[i],t[i],Ng_i,geomID,primID)) return true;
          }
          return false;
        }
#endif
        return true;
      }

#if defined(__AVX__)

      static __forceinline const Vec3<float8> getV012(const float *const grid, const size_t offset0, const size_t offset1, const size_t offset2)
      {
        const float4 ra = loadu4f(grid + offset0);
        const float4 rb = loadu4f(grid + offset1);
	const float4 rc = loadu4f(grid + offset2); // FIXME: this accesses 1 element too much
        const float8 r0 = float8(ra,rb);
        const float8 r1 = float8(rb,rc);
        return Vec3<float8>(unpacklo(r0,r1),         // r00, r10, r01, r11, r10, r20, r11, r21   
                            shuffle<1,1,2,2>(r0),    // r01, r01, r02, r02, r11, r11, r12, r12
                            shuffle<0,1,1,2>(r1));   // r10, r11, r11, r12, r20, r21, r21, r22
      }

      static __forceinline Vec2<float8> decodeUV(const float8 &uv)
      {
	const int8 i_uv = cast(uv);
	const int8 i_u  = i_uv & 0xffff;
	const int8 i_v  = srl(i_uv,16);
	const float8 u    = (float8)i_u * float8(1.0f/0xFFFF);
	const float8 v    = (float8)i_v * float8(1.0f/0xFFFF);
	return Vec2<float8>(u,v);
      }
     
 
      static __forceinline void intersect1_precise_3x3(Ray& ray,
						       const float *const grid_x,
						       const float *const grid_y,
						       const float *const grid_z,
						       const float *const grid_uv,
						       const size_t line_offset,
						       Precalculations &pre,
                                                       Scene* scene)
      {
	const size_t offset0 = 0 * line_offset;
	const size_t offset1 = 1 * line_offset;
	const size_t offset2 = 2 * line_offset;

	const Vec3<float8> tri012_x = getV012(grid_x,offset0,offset1,offset2);
	const Vec3<float8> tri012_y = getV012(grid_y,offset0,offset1,offset2);
	const Vec3<float8> tri012_z = getV012(grid_z,offset0,offset1,offset2);

	const Vec3<float8> v0_org(tri012_x[0],tri012_y[0],tri012_z[0]);
	const Vec3<float8> v1_org(tri012_x[1],tri012_y[1],tri012_z[1]);
	const Vec3<float8> v2_org(tri012_x[2],tri012_y[2],tri012_z[2]);
        

	const Vec3<float8> O = ray.org;
	const Vec3<float8> D = ray.dir;

	const Vec3<float8> v0 = v0_org - O;
	const Vec3<float8> v1 = v1_org - O;
	const Vec3<float8> v2 = v2_org - O;

	const Vec3<float8> e0 = v2 - v0;
	const Vec3<float8> e1 = v0 - v1;	     
	const Vec3<float8> e2 = v1 - v2;	     

	/* calculate geometry normal and denominator */
	const Vec3<float8> Ng1 = cross(e1,e0);
	const Vec3<float8> Ng = Ng1+Ng1;
	const float8 den = dot(Ng,D);
	const float8 absDen = abs(den);
	const float8 sgnDen = signmsk(den);
        
	bool8 valid ( true );
	/* perform edge tests */
	const float8 U = dot(Vec3<float8>(cross(v2+v0,e0)),D) ^ sgnDen;
	valid &= U >= 0.0f;
	if (likely(none(valid))) return;
	const float8 V = dot(Vec3<float8>(cross(v0+v1,e1)),D) ^ sgnDen;
	valid &= V >= 0.0f;
	if (likely(none(valid))) return;
	const float8 W = dot(Vec3<float8>(cross(v1+v2,e2)),D) ^ sgnDen;

	valid &= W >= 0.0f;
	if (likely(none(valid))) return;
        
	/* perform depth test */
	const float8 T = dot(v0,Ng) ^ sgnDen;
	valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);
	if (unlikely(none(valid))) return;
        
	/* perform backface culling */
#if defined(RTCORE_BACKFACE_CULLING)
	valid &= den > float8(zero);
	if (unlikely(none(valid))) return;
#else
	valid &= den != float8(zero);
	if (unlikely(none(valid))) return;
#endif

        /* ray mask test */
        const size_t geomID = pre.patch->geom;
        const size_t primID = pre.patch->prim;
        Geometry* geometry = scene->get(geomID);
#if defined(RTCORE_RAY_MASK)
        if ((geometry->mask & ray.mask) == 0) return;
#endif
        
	/* calculate hit information */
	const float8 rcpAbsDen = rcp(absDen);
	float8 u =  U*rcpAbsDen;
	float8 v =  V*rcpAbsDen;
	const float8 t = T*rcpAbsDen;
        
#if !FORCE_TRIANGLE_UV
	const Vec3<float8> tri012_uv = getV012(grid_uv,offset0,offset1,offset2);	
	const Vec2<float8> uv0 = decodeUV(tri012_uv[0]);
	const Vec2<float8> uv1 = decodeUV(tri012_uv[1]);
	const Vec2<float8> uv2 = decodeUV(tri012_uv[2]);        
	const Vec2<float8> uv = u * uv1 + v * uv2 + (1.0f-u-v) * uv0;
	u = uv[0];
	v = uv[1];
#endif        
        size_t i = select_min(valid,t);

        /* intersection filter test */
#if defined(RTCORE_INTERSECTION_FILTER)
        if (unlikely(geometry->hasIntersectionFilter1())) 
        {
          while (true) 
          {
            /* call intersection filter function */
            Vec3fa Ng_i = Vec3fa(Ng.x[i],Ng.y[i],Ng.z[i]);
            if (runIntersectionFilter1(geometry,ray,u[i],v[i],t[i],Ng_i,geomID,primID)) {
              return;
            }
            valid[i] = 0;
            if (unlikely(none(valid))) return;
            i = select_min(valid,t);
          }
          return;
        }
#endif
	
	/* update hit information */
	ray.u         = u[i];
	ray.v         = v[i];
	ray.tfar      = t[i];
        ray.geomID    = pre.patch->geom;
        ray.primID    = pre.patch->prim;
        ray.Ng.x      = Ng.x[i];
        ray.Ng.y      = Ng.y[i];
        ray.Ng.z      = Ng.z[i];
      };

        static __forceinline bool occluded1_precise_3x3(Ray& ray,
							const float *const grid_x,
							const float *const grid_y,
							const float *const grid_z,
							const float *const grid_uv,
							const size_t line_offset,
                                                        Precalculations &pre,
                                                        Scene* scene)
      {
	const size_t offset0 = 0 * line_offset;
	const size_t offset1 = 1 * line_offset;
	const size_t offset2 = 2 * line_offset;

	const Vec3<float8> tri012_x = getV012(grid_x,offset0,offset1,offset2);
	const Vec3<float8> tri012_y = getV012(grid_y,offset0,offset1,offset2);
	const Vec3<float8> tri012_z = getV012(grid_z,offset0,offset1,offset2);

	const Vec3<float8> v0_org(tri012_x[0],tri012_y[0],tri012_z[0]);
	const Vec3<float8> v1_org(tri012_x[1],tri012_y[1],tri012_z[1]);
	const Vec3<float8> v2_org(tri012_x[2],tri012_y[2],tri012_z[2]);
        
        const Vec3<float8> O = ray.org;
        const Vec3<float8> D = ray.dir;
        
        const Vec3<float8> v0 = v0_org - O;
        const Vec3<float8> v1 = v1_org - O;
        const Vec3<float8> v2 = v2_org - O;
        
        const Vec3<float8> e0 = v2 - v0;
        const Vec3<float8> e1 = v0 - v1;	     
        const Vec3<float8> e2 = v1 - v2;	     
        
        /* calculate geometry normal and denominator */
        const Vec3<float8> Ng1 = cross(e1,e0);
        const Vec3<float8> Ng = Ng1+Ng1;
        const float8 den = dot(Ng,D);
        const float8 absDen = abs(den);
        const float8 sgnDen = signmsk(den);
        
        bool8 valid ( true );
        /* perform edge tests */
        const float8 U = dot(Vec3<float8>(cross(v2+v0,e0)),D) ^ sgnDen;
        valid &= U >= 0.0f;
        if (likely(none(valid))) return false;
        const float8 V = dot(Vec3<float8>(cross(v0+v1,e1)),D) ^ sgnDen;
        valid &= V >= 0.0f;
        if (likely(none(valid))) return false;
        const float8 W = dot(Vec3<float8>(cross(v1+v2,e2)),D) ^ sgnDen;
        valid &= W >= 0.0f;
        if (likely(none(valid))) return false;
        
        /* perform depth test */
        const float8 T = dot(v0,Ng) ^ sgnDen;
        valid &= (T >= absDen*ray.tnear) & (absDen*ray.tfar >= T);
        if (unlikely(none(valid))) return false;
        
        /* perform backface culling */
#if defined(RTCORE_BACKFACE_CULLING)
        valid &= den > float8(zero);
        if (unlikely(none(valid))) return false;
#else
        valid &= den != float8(zero);
        if (unlikely(none(valid))) return false;
#endif

        /* ray mask test */
        const size_t geomID = pre.patch->geom;
        const size_t primID = pre.patch->prim;
        Geometry* geometry = scene->get(geomID);
#if defined(RTCORE_RAY_MASK)
        if ((geometry->mask & ray.mask) == 0) return false;
#endif

        /* intersection filter test */
#if defined(RTCORE_INTERSECTION_FILTER)
        if (unlikely(geometry->hasOcclusionFilter1())) 
        {
          /* calculate hit information */
          const float8 rcpAbsDen = rcp(absDen);
          float8 u =  U*rcpAbsDen;
          float8 v =  V*rcpAbsDen;
          const float8 t = T*rcpAbsDen;
        
#if !FORCE_TRIANGLE_UV
          const Vec3<float8> tri012_uv = getV012(grid_uv,offset0,offset1);	
          const Vec2<float8> uv0 = decodeUV(tri012_uv[0]);
          const Vec2<float8> uv1 = decodeUV(tri012_uv[1]);
          const Vec2<float8> uv2 = decodeUV(tri012_uv[2]);        
          const Vec2<float8> uv = u * uv1 + v * uv2 + (1.0f-u-v) * uv0;        
          u = uv[0];
          v = uv[1];        
#endif
          
          for (size_t m=movemask(valid), i=__bsf(m); m!=0; m=__btc(m,i), i=__bsf(m)) {  
            const Vec3fa Ng_i = Vec3fa(Ng.x[i],Ng.y[i],Ng.z[i]);
            if (runOcclusionFilter1(geometry,ray,u[i],v[i],t[i],Ng_i,geomID,primID)) return true;
          }
          return false;
        }
#endif

        return true;
      };

#endif      
      
      /*! Intersect a ray with the primitive. */
      static __forceinline void intersect(Precalculations& pre, Ray& ray, const Primitive* prim, size_t ty, Scene* scene, size_t& lazy_node) 
      {
        const size_t dim_offset    = pre.patch->grid_size_simd_blocks * vfloat::size;
        const size_t line_offset   = pre.patch->grid_u_res;
        const size_t offset_bytes  = ((size_t)prim  - (size_t)SharedLazyTessellationCache::sharedLazyTessellationCache.getDataPtr()) >> 2;   
        const float *const grid_x  = (float*)(offset_bytes + (size_t)SharedLazyTessellationCache::sharedLazyTessellationCache.getDataPtr());
        const float *const grid_y  = grid_x + 1 * dim_offset;
        const float *const grid_z  = grid_x + 2 * dim_offset;
        const float *const grid_uv = grid_x + 3 * dim_offset;
#if defined(__AVX__)
        intersect1_precise_3x3( ray, grid_x,grid_y,grid_z,grid_uv, line_offset, pre, scene);
#else
        intersect1_precise_2x3( ray, grid_x            ,grid_y            ,grid_z            ,grid_uv            , line_offset, pre, scene);
        intersect1_precise_2x3( ray, grid_x+line_offset,grid_y+line_offset,grid_z+line_offset,grid_uv+line_offset, line_offset, pre, scene);
#endif
      }
      
      /*! Test if the ray is occluded by the primitive */
      static __forceinline bool occluded(Precalculations& pre, Ray& ray, const Primitive* prim, size_t ty, Scene* scene, size_t& lazy_node) 
      {
        const size_t dim_offset    = pre.patch->grid_size_simd_blocks * vfloat::size;
        const size_t line_offset   = pre.patch->grid_u_res;
        const size_t offset_bytes  = ((size_t)prim  - (size_t)SharedLazyTessellationCache::sharedLazyTessellationCache.getDataPtr()) >> 2;   
        const float *const grid_x  = (float*)(offset_bytes + (size_t)SharedLazyTessellationCache::sharedLazyTessellationCache.getDataPtr());
        const float *const grid_y  = grid_x + 1 * dim_offset;
        const float *const grid_z  = grid_x + 2 * dim_offset;
        const float *const grid_uv = grid_x + 3 * dim_offset;

#if defined(__AVX__)
        return occluded1_precise_3x3( ray, grid_x,grid_y,grid_z,grid_uv, line_offset, pre, scene);
#else
        if (occluded1_precise_2x3( ray, grid_x            ,grid_y            ,grid_z            ,grid_uv            , line_offset, pre, scene)) return true;
        if (occluded1_precise_2x3( ray, grid_x+line_offset,grid_y+line_offset,grid_z+line_offset,grid_uv+line_offset, line_offset, pre, scene)) return true;
#endif
        return false;
      }      
    };
  }
}