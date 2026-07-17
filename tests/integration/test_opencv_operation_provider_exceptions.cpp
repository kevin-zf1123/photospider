#include <gtest/gtest.h>

#include <new>
#include <opencv2/core.hpp>
#include <string>
#include <typeinfo>

#include "core/ps_types.hpp"  // NOLINT(build/include_subdir)
#include "photospider/core/graph_error.hpp"
#include "providers/opencv/opencv_operation_provider.hpp"
#include "providers/opencv/opencv_operation_provider_test_access.hpp"

namespace ps::providers::opencv {
namespace {

/**
 * @brief Proves initialization retry and both provider exception fences.
 *
 * @throws Nothing when all GTest assertions pass; tested exceptions are caught
 *         and inspected inside the case.
 * @note This binary contains one test so its first `register_provider()` call
 *       is guaranteed to precede successful OpenCV provider initialization.
 *       The private hooks are compiled out of production builds and inject
 *       OpenCV status objects rather than attempting real resource exhaustion.
 */
TEST(OpenCvOperationProviderExceptionContract,
     InitializationRetryAndFencesTranslate) {
  constexpr char kResizeType[] = "image_process";
  constexpr char kResizeSubtype[] = "resize";

  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kResizeType, kResizeSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());

  set_opencv_process_initialization_failure_for_testing(cv::Error::StsError);
  bool initialization_translated = false;
  try {
    register_provider();
    ADD_FAILURE() << "injected OpenCV initialization unexpectedly succeeded";
  } catch (const GraphError& error) {
    initialization_translated = true;
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
    EXPECT_NE(std::string(error.what()).find("initialization"),
              std::string::npos);
  } catch (const cv::Exception& error) {
    ADD_FAILURE() << "OpenCV initialization exception escaped: "
                  << error.what();
  } catch (const std::exception& error) {
    ADD_FAILURE() << "unexpected initialization exception: " << error.what();
  } catch (...) {
    ADD_FAILURE() << "unknown initialization exception";
  }
  EXPECT_TRUE(initialization_translated);
  EXPECT_FALSE(OpRegistry::instance()
                   .resolve_for_intent(kResizeType, kResizeSubtype,
                                       ComputeIntent::GlobalHighPrecision)
                   .has_value());

  EXPECT_NO_THROW(register_provider());
  EXPECT_TRUE(OpRegistry::instance()
                  .resolve_for_intent(kResizeType, kResizeSubtype,
                                      ComputeIntent::GlobalHighPrecision)
                  .has_value());
  EXPECT_EQ(cv::getNumThreads(), 1);

  int exact_bad_alloc_count = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    try {
      invoke_monolithic_opencv_exception_fence_for_testing(cv::Error::StsNoMem);
      ADD_FAILURE() << "injected OpenCV exhaustion unexpectedly returned";
    } catch (const std::bad_alloc& error) {
      ++exact_bad_alloc_count;
      EXPECT_EQ(typeid(error), typeid(std::bad_alloc));
    } catch (const cv::Exception& error) {
      ADD_FAILURE() << "OpenCV exhaustion exception escaped: " << error.what();
    } catch (const std::exception& error) {
      ADD_FAILURE() << "unexpected exhaustion exception: " << error.what();
    } catch (...) {
      ADD_FAILURE() << "unknown exhaustion exception";
    }
  }
  EXPECT_EQ(exact_bad_alloc_count, 2);

  bool tiled_error_translated = false;
  try {
    invoke_tiled_opencv_exception_fence_for_testing(cv::Error::StsBadArg);
    ADD_FAILURE() << "injected tiled OpenCV failure unexpectedly returned";
  } catch (const GraphError& error) {
    tiled_error_translated = true;
    EXPECT_EQ(error.code(), GraphErrc::ComputeError);
    EXPECT_NE(std::string(error.what()).find("testing:tiled_exception_fence"),
              std::string::npos);
  } catch (const cv::Exception& error) {
    ADD_FAILURE() << "tiled OpenCV exception escaped: " << error.what();
  } catch (const std::exception& error) {
    ADD_FAILURE() << "unexpected tiled exception: " << error.what();
  } catch (...) {
    ADD_FAILURE() << "unknown tiled exception";
  }
  EXPECT_TRUE(tiled_error_translated);
}

}  // namespace
}  // namespace ps::providers::opencv
