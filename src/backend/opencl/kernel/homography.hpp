/*******************************************************
 * Copyright (c) 2015, ArrayFire
 * All rights reserved.
 *
 * This file is distributed under 3-clause BSD license.
 * The complete license agreement can be obtained at:
 * http://arrayfire.com/licenses/BSD-3-Clause
 ********************************************************/

#include <af/defines.h>
#include <dispatch.hpp>
#include <err_opencl.hpp>
#include <debug_opencl.hpp>
#include <memory.hpp>
#include <kernel_headers/homography.hpp>
#include <kernel/ireduce.hpp>
#include <kernel/reduce.hpp>
#include <kernel/sort.hpp>
#include <cfloat>

using cl::Buffer;
using cl::Program;
using cl::Kernel;
using cl::EnqueueArgs;
using cl::LocalSpaceArg;
using cl::NDRange;
using std::vector;

namespace opencl
{

namespace kernel
{

const int HG_THREADS_X = 16;
const int HG_THREADS_Y = 16;
const int HG_THREADS   = 256;

template<typename T, af_homography_type htype>
int computeH(
    Param bestH,
    Param H,
    Param A,
    Param V,
    Param err,
    Param x_src,
    Param y_src,
    Param x_dst,
    Param y_dst,
    Param rnd,
    const unsigned iterations,
    const unsigned nsamples,
    const float inlier_thr)
{
    try {
        static std::once_flag compileFlags[DeviceManager::MAX_DEVICES];
        static std::map<int, Program*> hgProgs;
        static std::map<int, Kernel*>  chKernel;
        static std::map<int, Kernel*>  ehKernel;
        static std::map<int, Kernel*>  cmKernel;
        static std::map<int, Kernel*>  fmKernel;
        static std::map<int, Kernel*>  clKernel;

        int device = getActiveDeviceId();

        std::call_once( compileFlags[device], [device] () {

                std::ostringstream options;
                options << " -D T=" << dtype_traits<T>::getName();

                if (std::is_same<T, double>::value) {
                    options << " -D USE_DOUBLE";
                    options << " -D EPS=" << DBL_EPSILON;
                } else
                    options << " -D EPS=" << FLT_EPSILON;

                if (htype == AF_HOMOGRAPHY_RANSAC)
                    options << " -D RANSAC";
                else if (htype == AF_HOMOGRAPHY_LMEDS)
                    options << " -D LMEDS";

                cl::Program prog;
                buildProgram(prog, homography_cl, homography_cl_len, options.str());
                hgProgs[device] = new Program(prog);

                chKernel[device] = new Kernel(*hgProgs[device], "compute_homography");
                ehKernel[device] = new Kernel(*hgProgs[device], "eval_homography");
                cmKernel[device] = new Kernel(*hgProgs[device], "compute_median");
                fmKernel[device] = new Kernel(*hgProgs[device], "find_min_median");
                clKernel[device] = new Kernel(*hgProgs[device], "compute_lmeds_inliers");
            });

        const int blk_x_ch = 1;
        const int blk_y_ch = divup(iterations, HG_THREADS_Y);
        const NDRange local_ch(HG_THREADS_X, HG_THREADS_Y);
        const NDRange global_ch(blk_x_ch * HG_THREADS_X, blk_y_ch * HG_THREADS_Y);

        // Build linear system and solve SVD
        auto chOp = make_kernel<Buffer, KParam, Buffer, KParam,
                                Buffer, KParam,
                                Buffer, Buffer, Buffer, Buffer,
                                Buffer, KParam, unsigned>(*chKernel[device]);

        chOp(EnqueueArgs(getQueue(), global_ch, local_ch),
             *H.data, H.info, *A.data, A.info,
             *V.data, V.info,
             *x_src.data, *y_src.data, *x_dst.data, *y_dst.data,
             *rnd.data, rnd.info, iterations);
        CL_DEBUG_FINISH(getQueue());

        const int blk_x_eh = divup(iterations, HG_THREADS);
        const NDRange local_eh(HG_THREADS);
        const NDRange global_eh(blk_x_eh * HG_THREADS);

        // Allocate some temporary buffers
        Param inliers, idx, median;
        inliers.info.offset = idx.info.offset = median.info.offset = 0;
        inliers.info.dims[0] = (htype == AF_HOMOGRAPHY_RANSAC) ? blk_x_eh : divup(nsamples, HG_THREADS);
        inliers.info.strides[0] = 1;
        idx.info.dims[0] = median.info.dims[0] = blk_x_eh;
        idx.info.strides[0] = median.info.strides[0] = 1;
        for (int k = 1; k < 4; k++) {
            inliers.info.dims[k] = 1;
            inliers.info.strides[k] = inliers.info.dims[k-1] * inliers.info.strides[k-1];
            idx.info.dims[k] = median.info.dims[k] = 1;
            idx.info.strides[k] = median.info.strides[k] = idx.info.dims[k-1] * idx.info.strides[k-1];
        }
        idx.data = bufferAlloc(idx.info.dims[3] * idx.info.strides[3] * sizeof(unsigned));
        inliers.data = bufferAlloc(inliers.info.dims[3] * inliers.info.strides[3] * sizeof(unsigned));
        if (htype == AF_HOMOGRAPHY_LMEDS)
            median.data = bufferAlloc(median.info.dims[3] * median.info.strides[3] * sizeof(float));
        else
            median.data = bufferAlloc(sizeof(float));

        // Compute (and for RANSAC, evaluate) homographies
        auto ehOp = make_kernel<Buffer, Buffer, Buffer, KParam,
                                Buffer, KParam,
                                Buffer, Buffer, Buffer, Buffer,
                                Buffer, unsigned, unsigned, float>(*ehKernel[device]);

        ehOp(EnqueueArgs(getQueue(), global_eh, local_eh),
             *inliers.data, *idx.data, *H.data, H.info,
             *err.data, err.info,
             *x_src.data, *y_src.data, *x_dst.data, *y_dst.data,
             *rnd.data, iterations, nsamples, inlier_thr);
        CL_DEBUG_FINISH(getQueue());

        unsigned inliersH, idxH;
        if (htype == AF_HOMOGRAPHY_LMEDS) {
            // TODO: Improve this sorting, if the number of iterations is
            // sufficiently large, this can be *very* slow
            kernel::sort0<float, true>(err);

            unsigned minIdx;
            float minMedian;

            // Compute median of every iteration
            auto cmOp = make_kernel<Buffer, Buffer, Buffer, KParam,
                                    unsigned>(*cmKernel[device]);

            cmOp(EnqueueArgs(getQueue(), global_eh, local_eh),
                 *median.data, *idx.data, *err.data, err.info,
                 iterations);
            CL_DEBUG_FINISH(getQueue());

            // Reduce medians, only in case iterations > 256
            if (blk_x_eh > 1) {
                const NDRange local_fm(HG_THREADS);
                const NDRange global_fm(HG_THREADS);

                cl::Buffer* finalMedian = bufferAlloc(sizeof(float));
                cl::Buffer* finalIdx = bufferAlloc(sizeof(unsigned));

                auto fmOp = make_kernel<Buffer, Buffer, Buffer, KParam,
                                        Buffer>(*fmKernel[device]);

                fmOp(EnqueueArgs(getQueue(), global_fm, local_fm),
                     *finalMedian, *finalIdx, *median.data, median.info,
                     *idx.data);
                CL_DEBUG_FINISH(getQueue());

                getQueue().enqueueReadBuffer(*finalMedian, CL_TRUE, 0, sizeof(float), &minMedian);
                getQueue().enqueueReadBuffer(*finalIdx, CL_TRUE, 0, sizeof(unsigned), &minIdx);

                bufferFree(finalMedian);
                bufferFree(finalIdx);
            }
            else {
                getQueue().enqueueReadBuffer(*median.data, CL_TRUE, 0, sizeof(float), &minMedian);
                getQueue().enqueueReadBuffer(*idx.data, CL_TRUE, 0, sizeof(unsigned), &minIdx);
            }

            // Copy best homography to output
            getQueue().enqueueCopyBuffer(*H.data, *bestH.data, minIdx*9*sizeof(T), 0, 9*sizeof(T));

            const int blk_x_cl = divup(nsamples, HG_THREADS);
            const NDRange local_cl(HG_THREADS);
            const NDRange global_cl(blk_x_cl * HG_THREADS);

            auto clOp = make_kernel<Buffer, Buffer,
                                    Buffer, Buffer, Buffer, Buffer,
                                    float, unsigned>(*clKernel[device]);

            clOp(EnqueueArgs(getQueue(), global_cl, local_cl),
                 *inliers.data, *bestH.data,
                 *x_src.data, *y_src.data, *x_dst.data, *y_dst.data,
                 minMedian, nsamples);
            CL_DEBUG_FINISH(getQueue());

            // Adds up the total number of inliers
            Param totalInliers;
            totalInliers.info.offset = 0;
            for (int k = 0; k < 4; k++)
                totalInliers.info.dims[k] = totalInliers.info.strides[k] = 1;
            totalInliers.data = bufferAlloc(sizeof(unsigned));

            kernel::reduce<unsigned, unsigned, af_add_t>(totalInliers, inliers, 0, false, 0.0);

            getQueue().enqueueReadBuffer(*totalInliers.data, CL_TRUE, 0, sizeof(unsigned), &inliersH);

            bufferFree(totalInliers.data);
        }
        else if (htype == AF_HOMOGRAPHY_RANSAC) {
            Param bestInliers, bestIdx;
            bestInliers.info.offset = bestIdx.info.offset = 0;
            for (int k = 0; k < 4; k++) {
                bestInliers.info.dims[k] = bestIdx.info.dims[k] = 1;
                bestInliers.info.strides[k] = bestIdx.info.strides[k] = 1;
            }
            bestInliers.data = bufferAlloc(sizeof(unsigned));
            bestIdx.data = bufferAlloc(sizeof(unsigned));

            kernel::ireduce<unsigned, af_max_t>(bestInliers, bestIdx.data, inliers, 0);

            unsigned blockIdx;
            getQueue().enqueueReadBuffer(*bestIdx.data, CL_TRUE, 0, sizeof(unsigned), &blockIdx);

            // Copies back index and number of inliers of best homography estimation
            getQueue().enqueueReadBuffer(*idx.data, CL_TRUE, blockIdx*sizeof(unsigned), sizeof(unsigned), &idxH);
            getQueue().enqueueReadBuffer(*bestInliers.data, CL_TRUE, 0, sizeof(unsigned), &inliersH);

            getQueue().enqueueCopyBuffer(*H.data, *bestH.data, idxH*9*sizeof(T), 0, 9*sizeof(T));

            bufferFree(bestInliers.data);
            bufferFree(bestIdx.data);
        }

        bufferFree(inliers.data);
        bufferFree(idx.data);
        bufferFree(median.data);

        return (int)inliersH;
    } catch (cl::Error err) {
        CL_TO_AF_ERROR(err);
        throw;
    }
}

} // namespace kernel

} // namespace cuda
