/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#if GOOGLE_CUDA || TENSORFLOW_USE_ROCM

#define EIGEN_USE_GPU

#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/kernels/training_ops.h"
#include "tensorflow/core/util/gpu_kernel_helper.h"

namespace tensorflow {

typedef Eigen::GpuDevice GPUDevice;

namespace functor {
template <typename T>
struct ApplyGradientDescent<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstFlat grad) {
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    var.device(d) -= lr.reshape(single).broadcast(bcast) * grad;
  }
};

#if TENSORFLOW_USE_ROCM

// if any kernels involving complex sqrt/rsqrt are compiled with ROCm, build process completes without errors,
// but the resulting executable ends up unusable (throwing errors "no device code available for function" 
// for completely unrelated kernels.)
// We have no choice but to implement sqrt and rsqrt by hand

template <typename T>
__device__ T impl_sqrt(T x) { return sqrt(x); }
template <typename T>
__device__ T impl_rsqrt(T x) { return rsqrt(x); }
template <>
__device__ Eigen::half impl_sqrt(Eigen::half x) { return __float2half(sqrt(__half2float(x))); }
template <>
__device__ Eigen::half impl_rsqrt(Eigen::half x) { return __float2half(rsqrt(__half2float(x))); }

template <class T>
__device__ std::complex<T> impl_sqrt(std::complex<T> x)
{
  T re = x.real(), im = x.imag();
  T mod_x = sqrt(re*re+im*im);
  const T root2 = 0.7071067811865475;
  // we pick the root with the same sign of the imaginary component as the input
  T root[2]={T(sqrt(mod_x+re)*root2), T(sqrt(mod_x-re)*root2*(im>=0 ? 1. : -1.))};
  // hcc/clang is really weird with its support of complex in device code;
  // for some reason it does not permit a 2-argument constructor
  return *(reinterpret_cast<std::complex<T>* >(&root));
}

template <class T>
__device__ T rsqrt_helper(T x)
{
  return 0.5*x + 0.125*x*x + 0.0625*x*x*x;
}

template <class T>
__device__ std::complex<T> impl_rsqrt(std::complex<T> x)
{
  T re = x.real(), im = x.imag();
  T r = rsqrt(re*re+im*im);
  T ar2 = re*r*r;
  const T root2 = 0.7071067811865475;
  T root[2];
  // With float, calculating 1+re*r and 1-re*r may result in excessive errors 
  // due to subtraction of two close values. We have to get fancy
  root[0] = sqrt(r * ((std::is_same<T,float>::value && re*r<-0.98) ? rsqrt_helper(im*im*r*r) : 1+re*r)) * root2;
  root[1] = sqrt(r * ((std::is_same<T,float>::value &&  re*r>0.98) ? rsqrt_helper(im*im*r*r) : 1-re*r)) * root2 * (im>=0 ? -1. : 1.);
  return *(reinterpret_cast<std::complex<T>* >(&root));
}

template <typename T>
__global__ void ApplyAdagradKernel(GpuLaunchConfig cfg, 
          T* var, T* accum, 
          const T* lr, 
          const T* grad, bool update_slots)
{
  GPU_1D_KERNEL_LOOP(i, cfg.virtual_thread_count) {
      if(update_slots)
        accum[i]+=grad[i]*grad[i];
      var[i] -= lr[0] * grad[i] * impl_rsqrt(accum[i]);
  }
}

template <typename T>
__global__ void ApplyAdagradV2Kernel(GpuLaunchConfig cfg, 
            T* var, T* accum, 
            const T* lr, 
            const T* epsilon, 
            const T* grad, bool update_slots)
{
  GPU_1D_KERNEL_LOOP(i, cfg.virtual_thread_count) {
      if(update_slots)
        accum[i]+=grad[i]*grad[i];
      T update = grad[i] / (impl_sqrt(accum[i]) + epsilon[0]);
      var[i] -= lr[0] * update;
  }
}

template <typename T>
__global__ void ApplyAdadeltaKernel(GpuLaunchConfig cfg, T* var,
                  T* accum,
                  T* accum_update,
                  const T* plr,
                  const T* prho,
                  const T* peps,
                  const T* grad)
{
  T rho = prho[0];
  T eps = peps[0];
  T lr = plr[0];
  GPU_1D_KERNEL_LOOP(i, cfg.virtual_thread_count) {
      accum[i] = accum[i]*rho + grad[i]*grad[i]*(T(1.0)-rho);
      T update = impl_sqrt(accum_update[i] + eps) * grad[i] * impl_rsqrt(accum[i]+eps);
      var[i] -= update*lr;
      accum_update[i] = accum_update[i]*rho + update*update*(T(1.0)-rho);
  }
}

template <typename T>
__global__ void ApplyRMSPropKernel(GpuLaunchConfig cfg, T* var,
                  T* ms, T* mom,
                  const T* plr,
                  const T* prho,
                  const T* pmomentum,
                  const T* peps,
                  const T* grad)
{
  T rho = prho[0];
  T eps = peps[0];
  T lr = plr[0];
  T momentum = pmomentum[0];
  GPU_1D_KERNEL_LOOP(i, cfg.virtual_thread_count) {
    ms[i] += (T(1.0)-rho)*(grad[i]*grad[i]-ms[i]);
    mom[i] = mom[i]*momentum + lr * grad[i] * impl_rsqrt(eps+ms[i]);
    var[i] -= mom[i];
  }
}

template <typename T>
__global__ void ApplyCenteredRMSPropKernel(GpuLaunchConfig cfg, T* var,
                  T* mg, T* ms, T* mom,
                  const T* plr,
                  const T* prho,
                  const T* pmomentum,
                  const T* peps,
                  const T* grad)
{
  T rho = prho[0];
  T eps = peps[0];
  T lr = plr[0];
  T momentum = pmomentum[0];
  T one_minus_rho = T(1.0)-rho;
  GPU_1D_KERNEL_LOOP(i, cfg.virtual_thread_count) {
    ms[i] += one_minus_rho*(grad[i]*grad[i]-ms[i]);
    mg[i] += one_minus_rho*(grad[i]-mg[i]);
    T denom = (ms[i]-mg[i]*mg[i])+eps;    
    mom[i] = mom[i]*momentum + lr*grad[i] * impl_rsqrt(denom);
    var[i] -= mom[i];
  }
}

#endif

template <typename T>
struct ApplyAdagrad<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat accum,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstFlat grad, bool update_slots) {
#if TENSORFLOW_USE_ROCM
    int32 data_dim = grad.dimension(0);
    auto config = GetGpuLaunchConfig(data_dim, d);

    TF_CHECK_OK(GpuLaunchKernel(
        ApplyAdagradKernel<T>, config.block_count, config.thread_per_block, 0,
        d.stream(), config, 
        var.data(),accum.data(), lr.data(), grad.data(), update_slots));
#else
    if (update_slots) {
      accum.device(d) += grad.square();
    }
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    var.device(d) -= lr.reshape(single).broadcast(bcast) * grad * accum.rsqrt();
#endif  
  }
};

template <typename T>
struct ApplyAdagradV2<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat accum,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar epsilon,
                  typename TTypes<T>::ConstFlat grad, bool update_slots) {
#if TENSORFLOW_USE_ROCM
    int32 data_dim = grad.dimension(0);
    auto config = GetGpuLaunchConfig(data_dim, d);

    TF_CHECK_OK(GpuLaunchKernel(
        ApplyAdagradV2Kernel<T>, config.block_count, config.thread_per_block, 0,
        d.stream(), config, 
        var.data(),accum.data(), lr.data(), epsilon.data(), grad.data(), update_slots));
#else
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    if (update_slots) {
      accum.device(d) += grad.square();
    }
    const auto update =
        grad / (accum.sqrt() + epsilon.reshape(single).broadcast(bcast));
    var.device(d) -= lr.reshape(single).broadcast(bcast) * update;
#endif
  }
};

template <typename T>
struct ApplyAdadelta<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat accum,
                  typename TTypes<T>::Flat accum_update,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar rho,
                  typename TTypes<T>::ConstScalar epsilon,
                  typename TTypes<T>::ConstFlat grad) {
#if TENSORFLOW_USE_ROCM
    int32 data_dim = grad.dimension(0);
    auto config = GetGpuLaunchConfig(data_dim, d);

    TF_CHECK_OK(GpuLaunchKernel(
        ApplyAdadeltaKernel<T>, config.block_count, config.thread_per_block, 0,
        d.stream(), config, 
        var.data(),accum.data(), accum_update.data(), 
        lr.data(), rho.data(), epsilon.data(), grad.data()));
#else
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;

    accum.device(d) = accum * rho.reshape(single).broadcast(bcast) +
                      grad.square() * (grad.constant(T(1)) -
                                       rho.reshape(single).broadcast(bcast));
    const auto update =
        (accum_update + epsilon.reshape(single).broadcast(bcast)).sqrt() *
        (accum + epsilon.reshape(single).broadcast(bcast)).rsqrt() * grad;
    var.device(d) -= update * lr.reshape(single).broadcast(bcast);
    accum_update.device(d) =
        accum_update * rho.reshape(single).broadcast(bcast) +
        update.square() *
            (grad.constant(T(1)) - rho.reshape(single).broadcast(bcast));
#endif  
  }
};

template <typename T>
struct ApplyMomentum<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat accum,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstFlat grad,
                  typename TTypes<T>::ConstScalar momentum, bool use_nesterov) {
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    accum.device(d) = accum * momentum.reshape(single).broadcast(bcast) + grad;
    if (use_nesterov) {
      var.device(d) -= grad * lr.reshape(single).broadcast(bcast) +
                       accum * momentum.reshape(single).broadcast(bcast) *
                           lr.reshape(single).broadcast(bcast);
    } else {
      var.device(d) -= lr.reshape(single).broadcast(bcast) * accum;
    }
  }
};

template <typename T>
struct ApplyKerasMomentum<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat accum,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstFlat grad,
                  typename TTypes<T>::ConstScalar momentum, bool use_nesterov) {
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    accum.device(d) = (accum * momentum.reshape(single).broadcast(bcast) -
                       grad * lr.reshape(single).broadcast(bcast));
    if (use_nesterov) {
      var.device(d) += (accum * momentum.reshape(single).broadcast(bcast) -
                        grad * lr.reshape(single).broadcast(bcast));
    } else {
      var.device(d) += accum;
    }
  }
};

template <typename T>
struct ApplyAdam<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat m, typename TTypes<T>::Flat v,
                  typename TTypes<T>::ConstScalar beta1_power,
                  typename TTypes<T>::ConstScalar beta2_power,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar beta1,
                  typename TTypes<T>::ConstScalar beta2,
                  typename TTypes<T>::ConstScalar epsilon,
                  typename TTypes<T>::ConstFlat grad, bool use_nesterov) {
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    const auto one = static_cast<T>(1.0);
    m.device(d) =
        m + (beta1.constant(one) - beta1).reshape(single).broadcast(bcast) *
                (grad - m);
    v.device(d) =
        v + (beta2.constant(one) - beta2).reshape(single).broadcast(bcast) *
                (grad.square() - v);

    if (use_nesterov) {
      var.device(d) -=
          (lr * (beta2_power.constant(one) - beta2_power).sqrt() /
           (beta1_power.constant(one) - beta1_power))
              .reshape(single)
              .broadcast(bcast) *
          (m * beta1.reshape(single).broadcast(bcast) +
           (beta1.constant(one) - beta1).reshape(single).broadcast(bcast) *
               grad) /
          (epsilon.reshape(single).broadcast(bcast) + v.sqrt());
    } else {
      var.device(d) -= (lr * (beta2_power.constant(one) - beta2_power).sqrt() /
                        (beta1_power.constant(one) - beta1_power))
                           .reshape(single)
                           .broadcast(bcast) *
                       m /
                       (epsilon.reshape(single).broadcast(bcast) + v.sqrt());
    }
  }
};

template <typename T>
struct ApplyAdamWithAmsgrad<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat m, typename TTypes<T>::Flat v,
                  typename TTypes<T>::Flat vhat,
                  typename TTypes<T>::ConstScalar beta1_power,
                  typename TTypes<T>::ConstScalar beta2_power,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar beta1,
                  typename TTypes<T>::ConstScalar beta2,
                  typename TTypes<T>::ConstScalar epsilon,
                  typename TTypes<T>::ConstFlat grad) {
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    const auto one = static_cast<T>(1.0);
    m.device(d) =
        m + (beta1.constant(one) - beta1).reshape(single).broadcast(bcast) *
                (grad - m);
    v.device(d) =
        v + (beta2.constant(one) - beta2).reshape(single).broadcast(bcast) *
                (grad.square() - v);
    vhat.device(d) = vhat.cwiseMax(v);

    var.device(d) -= (lr * (beta2_power.constant(one) - beta2_power).sqrt() /
                      (beta1_power.constant(one) - beta1_power))
                         .reshape(single)
                         .broadcast(bcast) *
                     m /
                     (epsilon.reshape(single).broadcast(bcast) + vhat.sqrt());
  }
};

template <typename T>
struct ApplyAdaMax<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat m, typename TTypes<T>::Flat v,
                  typename TTypes<T>::ConstScalar beta1_power,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar beta1,
                  typename TTypes<T>::ConstScalar beta2,
                  typename TTypes<T>::ConstScalar epsilon,
                  typename TTypes<T>::ConstFlat grad) {
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    const auto one = static_cast<T>(1.0);
    m.device(d) =
        m + (beta1.constant(one) - beta1).reshape(single).broadcast(bcast) *
                (grad - m);
    v.device(d) =
        (beta2.reshape(single).broadcast(bcast) * v).cwiseMax(grad.abs());
    var.device(d) -=
        lr / (beta1_power.constant(one) -
                 beta1_power).reshape(single).broadcast(bcast) *
                     (m / (v + epsilon));
  }
};

template <typename T>
struct ApplyRMSProp<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat ms, typename TTypes<T>::Flat mom,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar rho,
                  typename TTypes<T>::ConstScalar momentum,
                  typename TTypes<T>::ConstScalar epsilon,
                  typename TTypes<T>::ConstFlat grad) {
#if TENSORFLOW_USE_ROCM
    int32 data_dim = grad.dimension(0);
    auto config = GetGpuLaunchConfig(data_dim, d);

    TF_CHECK_OK(GpuLaunchKernel(
        ApplyRMSPropKernel<T>, config.block_count, config.thread_per_block, 0,
        d.stream(), config,         
        var.data(), ms.data(), mom.data(), 
        lr.data(), rho.data(), momentum.data(), epsilon.data(), grad.data()));
#else    
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    const auto one = static_cast<T>(1.0);
    ms.device(d) =
        ms + (rho.constant(one) - rho).reshape(single).broadcast(bcast) *
                 (grad.square() - ms);
    mom.device(d) =
        mom * momentum.reshape(single).broadcast(bcast) +
        lr.reshape(single).broadcast(bcast) * grad /
            ((epsilon.reshape(single).broadcast(bcast) + ms).sqrt());
    var.device(d) -= mom;
#endif    
  }
};

template <typename T>
struct ApplyCenteredRMSProp<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat mg, typename TTypes<T>::Flat ms,
                  typename TTypes<T>::Flat mom,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar rho,
                  typename TTypes<T>::ConstScalar momentum,
                  typename TTypes<T>::ConstScalar epsilon,
                  typename TTypes<T>::ConstFlat grad) {
#if TENSORFLOW_USE_ROCM
    int32 data_dim = grad.dimension(0);
    auto config = GetGpuLaunchConfig(data_dim, d);

    TF_CHECK_OK(GpuLaunchKernel(
        ApplyCenteredRMSPropKernel<T>, config.block_count, config.thread_per_block, 0,
        d.stream(), config,         
        var.data(), mg.data(), ms.data(), mom.data(), 
        lr.data(), rho.data(), momentum.data(), epsilon.data(), grad.data()));
#else
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;
    const auto one = static_cast<T>(1.0);
    const auto one_minus_rho =
        (rho.constant(one) - rho).reshape(single).broadcast(bcast);
    ms.device(d) = ms + one_minus_rho * (grad.square() - ms);
    mg.device(d) = mg + one_minus_rho * (grad - mg);
    auto denom = (ms - mg.square()) + epsilon.reshape(single).broadcast(bcast);
    mom.device(d) = mom * momentum.reshape(single).broadcast(bcast) +
                    lr.reshape(single).broadcast(bcast) * grad / denom.sqrt();
    var.device(d) -= mom;
#endif
  }
};

template <typename T>
struct ApplyAddSign<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat m,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar alpha,
                  typename TTypes<T>::ConstScalar sign_decay,
                  typename TTypes<T>::ConstScalar beta,
                  typename TTypes<T>::ConstFlat grad) {
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;

    // The following is the GPU equivalent of the CPU version:
    // m.device(d) = m * beta() + grad * (static_cast<T>(1) - beta());
    const auto one = static_cast<T>(1.0);
    auto beta_bcast = beta.reshape(single).broadcast(bcast);
    auto one_minus_beta =
        (beta.constant(one) - beta).reshape(single).broadcast(bcast);
    m.device(d) = m * beta_bcast + grad * one_minus_beta;

    // The following is the GPU equivalent of the CPU version:
    // var.device(d) -= lr() * (alpha() + sign_decay() * sign_gm) * grad;
    auto sign_gm = grad.sign() * m.sign();
    auto lr_bcast = lr.reshape(single).broadcast(bcast);
    auto alpha_bcast = alpha.reshape(single).broadcast(bcast);
    auto sign_decay_bcast = sign_decay.reshape(single).broadcast(bcast);
    var.device(d) -=
        lr_bcast * (alpha_bcast + sign_decay_bcast * sign_gm) * grad;
  }
};

template <typename T>
struct ApplyPowerSign<GPUDevice, T> {
  void operator()(const GPUDevice& d, typename TTypes<T>::Flat var,
                  typename TTypes<T>::Flat m,
                  typename TTypes<T>::ConstScalar lr,
                  typename TTypes<T>::ConstScalar logbase,
                  typename TTypes<T>::ConstScalar sign_decay,
                  typename TTypes<T>::ConstScalar beta,
                  typename TTypes<T>::ConstFlat grad) {
    Eigen::array<typename TTypes<T>::Tensor::Index, 1> bcast;
    bcast[0] = grad.dimension(0);
    Eigen::Sizes<1> single;

    // The following is the GPU equivalent of the CPU version:
    // m.device(d) = m * beta() + grad * (static_cast<T>(1) - beta());
    const auto one = static_cast<T>(1.0);
    auto beta_bcast = beta.reshape(single).broadcast(bcast);
    auto one_minus_beta =
        (beta.constant(one) - beta).reshape(single).broadcast(bcast);
    m.device(d) = m * beta_bcast + grad * one_minus_beta;

    // The following is the GPU equivalent of the CPU version:
    // auto grad_scale = (logbase() * sign_decay() * sign_gm).exp();
    // var.device(d) -= lr() * grad_scale * grad;
    auto sign_gm = grad.sign() * m.sign();
    auto lr_bcast = lr.reshape(single).broadcast(bcast);
    auto logbase_bcast = logbase.reshape(single).broadcast(bcast);
    auto sign_decay_bcast = sign_decay.reshape(single).broadcast(bcast);
    auto grad_scale = (logbase_bcast * sign_decay_bcast * sign_gm).exp();
    var.device(d) -= lr_bcast * grad_scale * grad;
  }
};

}  // namespace functor

template struct functor::ApplyGradientDescent<GPUDevice, Eigen::half>;
template struct functor::ApplyGradientDescent<GPUDevice, float>;
template struct functor::ApplyGradientDescent<GPUDevice, double>;
#if !defined(TENSORFLOW_USE_NVCC)
#ifndef PLATFORM_WINDOWS
template struct functor::ApplyGradientDescent<GPUDevice, complex64>;
template struct functor::ApplyGradientDescent<GPUDevice, complex128>;
#endif
#endif

template struct functor::ApplyAdagrad<GPUDevice, Eigen::half>;
template struct functor::ApplyAdagrad<GPUDevice, float>;
template struct functor::ApplyAdagrad<GPUDevice, double>;
#if !defined(TENSORFLOW_USE_NVCC)
#ifndef PLATFORM_WINDOWS
template struct functor::ApplyAdagrad<GPUDevice, complex64>;
template struct functor::ApplyAdagrad<GPUDevice, complex128>;
#endif
#endif

template struct functor::ApplyAdagradV2<GPUDevice, Eigen::half>;
template struct functor::ApplyAdagradV2<GPUDevice, float>;
template struct functor::ApplyAdagradV2<GPUDevice, double>;
#if !defined(TENSORFLOW_USE_NVCC)
#ifndef PLATFORM_WINDOWS
template struct functor::ApplyAdagradV2<GPUDevice, complex64>;
template struct functor::ApplyAdagradV2<GPUDevice, complex128>;
#endif
#endif

template struct functor::ApplyAdadelta<GPUDevice, Eigen::half>;
template struct functor::ApplyAdadelta<GPUDevice, float>;
template struct functor::ApplyAdadelta<GPUDevice, double>;
#if !defined(TENSORFLOW_USE_NVCC)
#ifndef PLATFORM_WINDOWS
template struct functor::ApplyAdadelta<GPUDevice, complex64>;
template struct functor::ApplyAdadelta<GPUDevice, complex128>;
#endif
#endif

template struct functor::ApplyFtrl<GPUDevice, Eigen::half>;
template struct functor::ApplyFtrl<GPUDevice, float>;
template struct functor::ApplyFtrl<GPUDevice, double>;

template struct functor::ApplyFtrlV2<GPUDevice, Eigen::half>;
template struct functor::ApplyFtrlV2<GPUDevice, float>;
template struct functor::ApplyFtrlV2<GPUDevice, double>;

template struct functor::ApplyMomentum<GPUDevice, Eigen::half>;
template struct functor::ApplyMomentum<GPUDevice, float>;
template struct functor::ApplyMomentum<GPUDevice, double>;
#if !defined(TENSORFLOW_USE_NVCC)  && !defined(TENSORFLOW_USE_ROCM)
#ifndef PLATFORM_WINDOWS
template struct functor::ApplyMomentum<GPUDevice, complex64>;
template struct functor::ApplyMomentum<GPUDevice, complex128>;
#endif
#endif

template struct functor::ApplyKerasMomentum<GPUDevice, Eigen::half>;
template struct functor::ApplyKerasMomentum<GPUDevice, float>;
template struct functor::ApplyKerasMomentum<GPUDevice, double>;
#if !defined(TENSORFLOW_USE_NVCC)  && !defined(TENSORFLOW_USE_ROCM)
#ifndef PLATFORM_WINDOWS
template struct functor::ApplyKerasMomentum<GPUDevice, complex64>;
template struct functor::ApplyKerasMomentum<GPUDevice, complex128>;
#endif
#endif

template struct functor::ApplyAdam<GPUDevice, Eigen::half>;
template struct functor::ApplyAdam<GPUDevice, float>;
template struct functor::ApplyAdam<GPUDevice, double>;
#if !defined(TENSORFLOW_USE_NVCC)  && !defined(TENSORFLOW_USE_ROCM)
#ifndef PLATFORM_WINDOWS
template struct functor::ApplyAdam<GPUDevice, complex64>;
template struct functor::ApplyAdam<GPUDevice, complex128>;
#endif
#endif

template struct functor::ApplyAdamWithAmsgrad<GPUDevice, Eigen::half>;
template struct functor::ApplyAdamWithAmsgrad<GPUDevice, float>;
template struct functor::ApplyAdamWithAmsgrad<GPUDevice, double>;

template struct functor::ApplyAdaMax<GPUDevice, Eigen::half>;
template struct functor::ApplyAdaMax<GPUDevice, float>;
template struct functor::ApplyAdaMax<GPUDevice, double>;

template struct functor::ApplyRMSProp<GPUDevice, Eigen::half>;
template struct functor::ApplyRMSProp<GPUDevice, float>;
template struct functor::ApplyRMSProp<GPUDevice, double>;
#if !defined(TENSORFLOW_USE_NVCC)
#ifndef PLATFORM_WINDOWS
template struct functor::ApplyRMSProp<GPUDevice, complex64>;
template struct functor::ApplyRMSProp<GPUDevice, complex128>;
#endif
#endif

template struct functor::ApplyCenteredRMSProp<GPUDevice, Eigen::half>;
template struct functor::ApplyCenteredRMSProp<GPUDevice, float>;
template struct functor::ApplyCenteredRMSProp<GPUDevice, double>;
#if !defined(TENSORFLOW_USE_NVCC)
#ifndef PLATFORM_WINDOWS
template struct functor::ApplyCenteredRMSProp<GPUDevice, complex64>;
template struct functor::ApplyCenteredRMSProp<GPUDevice, complex128>;
#endif
#endif

template struct functor::ApplyAddSign<GPUDevice, Eigen::half>;
template struct functor::ApplyAddSign<GPUDevice, float>;
template struct functor::ApplyAddSign<GPUDevice, double>;

template struct functor::ApplyPowerSign<GPUDevice, Eigen::half>;
template struct functor::ApplyPowerSign<GPUDevice, float>;
template struct functor::ApplyPowerSign<GPUDevice, double>;

}  // end namespace tensorflow

#endif  // GOOGLE_CUDA || TENSORFLOW_USE_ROCM
