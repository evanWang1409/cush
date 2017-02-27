#ifndef CUSH_SPHERICAL_HARMONICS_H_
#define CUSH_SPHERICAL_HARMONICS_H_

#define _USE_MATH_DEFINES

#include <math.h>

#include <device_launch_parameters.h>
#include <vector_types.h>

#include <clebsch_gordan.h>
#include <decorators.h>
#include <legendre.h>

// Based on "Spherical Harmonic Lighting: The Gritty Details" by Robin Green.
namespace cush
{ 
INLINE COMMON dim3         block_size_2d    ()
{
  return {16, 16, 1};
}
INLINE COMMON dim3         block_size_3d    ()
{
  return{8, 8, 8};
}

INLINE COMMON unsigned int maximum_degree   (const unsigned int coefficient_count)
{
  return sqrtf(coefficient_count) - 1;
}
INLINE COMMON unsigned int coefficient_count(const unsigned int max_l)
{
  return (max_l + 1) * (max_l + 1);
}
INLINE COMMON unsigned int coefficient_index(const unsigned int l, const int m)
{
  return l * (l + 1) + m;
}
INLINE COMMON int2         coefficient_lm   (const unsigned int index)
{
  int2 lm;
  lm.x = floor(sqrtf(index));
  lm.y = index - powf(lm.x, 2) - lm.x;
  return lm;
}

template<typename precision>
COMMON precision evaluate(
  const unsigned int l    ,
  const          int m    ,
  const precision&   theta,
  const precision&   phi  )
{
  precision kml = sqrt((2.0 * l + 1) * factorial<precision>(l - abs(m)) / 
                       (4.0 * M_PI   * factorial<precision>(l + abs(m))));
  if (m > 0)
    return sqrt(2.0) * kml * cos( m * theta) * associated_legendre(l,  m, cos(phi));
  if (m < 0)
    return sqrt(2.0) * kml * sin(-m * theta) * associated_legendre(l, -m, cos(phi));
  return kml * associated_legendre(l, 0, cos(phi));
}
template<typename precision>
COMMON precision evaluate(
  const unsigned int index,
  const precision&   theta,
  const precision&   phi  )
{
  auto lm = coefficient_lm(index);
  return evaluate(lm.x, lm.y, theta, phi);
}

// Not used internally as the two for loops can also be further parallelized.
template<typename precision>
COMMON precision evaluate_sum(
  const unsigned int max_l       ,
  const precision&   theta       ,
  const precision&   phi         ,
  const precision*   coefficients)
{
  precision sum = 0.0;
  for (int l = 0; l <= max_l; l++)
    for (int m = -l; m <= l; m++)
      sum += evaluate(l, m, theta, phi) * coefficients[coefficient_index(l, m)];
  return sum;
}

template<typename precision>
COMMON precision is_zero(
  const unsigned int coefficient_count,
  const precision*   coefficients )
{
  precision value = 0;
  for (auto index = 0; index < coefficient_count; index++)
    if (coefficients[index] != precision(0))
      return false;
  return true;
}

template<typename precision>
COMMON precision l1_distance(
  const unsigned int coefficient_count,
  const precision*   lhs_coefficients ,
  const precision*   rhs_coefficients )
{
  precision value = 0;
  for (auto index = 0; index < coefficient_count; index++)
    value += abs(lhs_coefficients[index] - rhs_coefficients[index]);
  return value;
}

// Based on "Rotation Invariant Spherical Harmonic Representation of 3D Shape Descriptors" by Kazhdan et al.
template<typename precision>
COMMON precision l2_distance(
  const unsigned int coefficient_count,
  const precision*   lhs_coefficients ,
  const precision*   rhs_coefficients )
{
  precision value = 0;
  for (auto index = 0; index < coefficient_count; index++)
    value += pow(lhs_coefficients[index] - rhs_coefficients[index], 2);
  return sqrt(value);
}

// Call on a vector_count x coefficient_count(max_l) 2D grid.
template<typename vector_type, typename precision>
GLOBAL void calculate_matrix(
  const unsigned int vector_count     ,
  const unsigned int coefficient_count,
  const vector_type* vectors          , 
  precision*         output_matrix    )
{
  auto vector_index      = blockIdx.x * blockDim.x + threadIdx.x;
  auto coefficient_index = blockIdx.y * blockDim.y + threadIdx.y;
  
  if (vector_index      >= vector_count     || 
      coefficient_index >= coefficient_count)
    return;

  atomicAdd(
    &output_matrix[vector_index + vector_count * coefficient_index], 
    evaluate(coefficient_index, vectors[vector_index].y, vectors[vector_index].z));
}
// Call on a dimensions.x x dimensions.y x dimensions.z 3D grid.
template<typename vector_type, typename precision>
GLOBAL void calculate_matrices(
  const uint3        dimensions       ,
  const unsigned int vector_count     , 
  const unsigned int coefficient_count,
  const vector_type* vectors          ,
  precision*         output_matrices  )
{
  auto x = blockIdx.x * blockDim.x + threadIdx.x;
  auto y = blockIdx.y * blockDim.y + threadIdx.y;
  auto z = blockIdx.z * blockDim.z + threadIdx.z;
  
  if (x >= dimensions.x || y >= dimensions.y || z >= dimensions.z)
    return;
  
  auto vectors_offset = vector_count  * (z + dimensions.z * (y + dimensions.y * x));
  auto matrix_offset  = vectors_offset * coefficient_count;
  
  dim3 block_size = block_size_2d();
  dim3 grid_size {
    ceil(float(vector_count     ) / block_size.x),
    ceil(float(coefficient_count) / block_size.y),
    1};

  calculate_matrix<<<grid_size, block_size>>>(
    vector_count     , 
    coefficient_count, 
    vectors         + vectors_offset, 
    output_matrices + matrix_offset );
}

// Call on a tessellations.x x tessellations.y 2D grid.
template<typename point_type>
GLOBAL void sample(
  const unsigned int l             ,
  const int          m             ,
  const uint2        tessellations ,
  point_type*        output_points ,
  unsigned int*      output_indices)
{
  auto longitude = blockIdx.x * blockDim.x + threadIdx.x;
  auto latitude  = blockIdx.y * blockDim.y + threadIdx.y;
  
  if (longitude >= tessellations.x ||
      latitude  >= tessellations.y )
    return;
  
  auto point_offset = latitude + longitude * tessellations.y;
  auto index_offset = 6 * point_offset;

  auto& point = output_points[point_offset];
  point.y = 2 * M_PI * longitude /  tessellations.x;
  point.z =     M_PI * latitude  / (tessellations.y - 1);
  point.x = evaluate(l, m, point.y, point.z);

  output_indices[index_offset    ] =  longitude                        * tessellations.y +  latitude,
  output_indices[index_offset + 1] =  longitude                        * tessellations.y + (latitude + 1) % tessellations.y,
  output_indices[index_offset + 2] = (longitude + 1) % tessellations.x * tessellations.y + (latitude + 1) % tessellations.y,
  output_indices[index_offset + 3] =  longitude                        * tessellations.y +  latitude,
  output_indices[index_offset + 4] = (longitude + 1) % tessellations.x * tessellations.y + (latitude + 1) % tessellations.y,
  output_indices[index_offset + 5] = (longitude + 1) % tessellations.x * tessellations.y +  latitude;
}
// Call on a tessellations.x x tessellations.y x coefficient_count(max_l) 3D grid.
template<typename precision, typename point_type>
GLOBAL void sample_sum(
  const unsigned int coefficient_count   ,
  const uint2        tessellations       ,
  const precision*   coefficients        ,
  point_type*        output_points       ,
  unsigned int*      output_indices      ,
  const unsigned int base_index          = 0)
{
  auto longitude         = blockIdx.x * blockDim.x + threadIdx.x;
  auto latitude          = blockIdx.y * blockDim.y + threadIdx.y;
  auto coefficient_index = blockIdx.z * blockDim.z + threadIdx.z;
  
  if (longitude         >= tessellations.x  ||
      latitude          >= tessellations.y  ||
      coefficient_index >= coefficient_count)
    return;

  auto point_offset = latitude + longitude * tessellations.y;
  auto index_offset = 6 * point_offset;

  auto& point = output_points[point_offset];

  if (coefficient_index == 0)
    point.x = 0;
  
  point.y = 2 * M_PI * longitude /  tessellations.x;
  point.z =     M_PI * latitude  / (tessellations.y - 1);
  atomicAdd(&point.x, evaluate(coefficient_index, point.y, point.z) * coefficients[coefficient_index]);

  if (coefficient_index == 0)
  {
    output_indices[index_offset    ] = base_index +  longitude                        * tessellations.y +  latitude,
    output_indices[index_offset + 1] = base_index +  longitude                        * tessellations.y + (latitude + 1) % tessellations.y,
    output_indices[index_offset + 2] = base_index + (longitude + 1) % tessellations.x * tessellations.y + (latitude + 1) % tessellations.y,
    output_indices[index_offset + 3] = base_index +  longitude                        * tessellations.y +  latitude,
    output_indices[index_offset + 4] = base_index + (longitude + 1) % tessellations.x * tessellations.y + (latitude + 1) % tessellations.y,
    output_indices[index_offset + 5] = base_index + (longitude + 1) % tessellations.x * tessellations.y +  latitude;
  }
}
// Call on a dimensions.x x dimensions.y x dimensions.z 3D grid.
template<typename precision, typename point_type>
GLOBAL void sample_sums(
  const uint3        dimensions         ,
  const unsigned int coefficient_count  ,
  const uint2        tessellations      ,
  const precision*   coefficients       ,
  point_type*        output_points      ,
  unsigned int*      output_indices     ,
  const unsigned int base_index         = 0)
{
  auto x = blockIdx.x * blockDim.x + threadIdx.x;
  auto y = blockIdx.y * blockDim.y + threadIdx.y;
  auto z = blockIdx.z * blockDim.z + threadIdx.z;
  
  if (x >= dimensions.x || 
      y >= dimensions.y || 
      z >= dimensions.z )
    return;
  
  auto volume_index        = z + dimensions.z * (y + dimensions.y * x);
  auto coefficients_offset = volume_index * coefficient_count;
  auto points_offset       = volume_index * tessellations.x * tessellations.y;
  auto indices_offset      = 6 * points_offset;

  dim3 block_size = block_size_3d();
  dim3 grid_size {
    ceil(float(tessellations.x  ) / block_size.x),
    ceil(float(tessellations.y  ) / block_size.y),
    ceil(float(coefficient_count) / block_size.z)};

  sample_sum<<<grid_size, block_size>>>(
    coefficient_count,
    tessellations    ,
    coefficients   + coefficients_offset, 
    output_points  + points_offset      ,
    output_indices + indices_offset     ,
    base_index     + points_offset      );
}

// Call on a coefficient_count x coefficient_count x coefficient_count 3D grid.
// Based on Modern Quantum Mechanics 2nd Edition page 216 by Jun John Sakurai.
template<typename precision, typename atomics_precision = float>
GLOBAL void product(
  const unsigned int coefficient_count,
  const precision*   lhs_coefficients ,
  const precision*   rhs_coefficients ,
  atomics_precision* out_coefficients )
{
  auto lhs_index = blockIdx.x * blockDim.x + threadIdx.x;
  auto rhs_index = blockIdx.y * blockDim.y + threadIdx.y;
  auto out_index = blockIdx.z * blockDim.z + threadIdx.z;
  
  if (lhs_index >= coefficient_count ||
      rhs_index >= coefficient_count ||
      out_index >= coefficient_count)
    return;

  auto lhs_lm   = coefficient_lm(lhs_index);
  auto rhs_lm   = coefficient_lm(rhs_index);
  auto out_lm   = coefficient_lm(out_index);
  auto cg1      = clebsch_gordan<atomics_precision>(lhs_lm.x, rhs_lm.x, out_lm.x, 0, 0, 0);
  auto cg2      = clebsch_gordan<atomics_precision>(lhs_lm.x, rhs_lm.x, out_lm.x, lhs_lm.y, rhs_lm.y, out_lm.y);
  auto coupling = sqrt((2 * lhs_lm.x + 1) * (2 * rhs_lm.x + 1) / (4 * M_PI * (2 * out_lm.x + 1))) * cg1 * cg2;

  atomicAdd(&out_coefficients[out_index], atomics_precision(coupling * lhs_coefficients[lhs_index] * rhs_coefficients[rhs_index]));
}
// Call on a dimensions.x x dimensions.y x dimensions.z 3D grid.
template<typename precision, typename atomics_precision = float>
GLOBAL void product(
  const uint3        dimensions       ,
  const unsigned int coefficient_count,
  const precision*   lhs_coefficients ,
  const precision*   rhs_coefficients ,
  atomics_precision* out_coefficients )
{
  auto x = blockIdx.x * blockDim.x + threadIdx.x;
  auto y = blockIdx.y * blockDim.y + threadIdx.y;
  auto z = blockIdx.z * blockDim.z + threadIdx.z;

  if (x >= dimensions.x || y >= dimensions.y || z >= dimensions.z)
    return;

  auto coefficients_offset = coefficient_count * (z + dimensions.z * (y + dimensions.y * x));
  
  dim3 block_size = block_size_3d();
  dim3 grid_size {
    ceil(float(coefficient_count) / block_size.x),
    ceil(float(coefficient_count) / block_size.y),
    ceil(float(coefficient_count) / block_size.z)};

  product<<<grid_size, block_size>>>(
    coefficient_count,
    lhs_coefficients + coefficients_offset,
    rhs_coefficients + coefficients_offset,
    out_coefficients + coefficients_offset);
}
}

#endif
