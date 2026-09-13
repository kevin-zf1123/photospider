#include <new>
#include <opencv2/core.hpp>
#include <opencv2/photo.hpp>

#include "09-composite/inpaint_ns.hpp"

namespace ps::plugin_internal::inpaint_ns {
void opencv(float* source, float* target, unsigned char* mask, int h, int w,
            int radius) {
  const cv::Mat input(h, w, CV_32FC1, source);
  const cv::Mat holes(h, w, CV_8UC1, mask);
  cv::Mat output(h, w, CV_32FC1, target);
  try {
    cv::inpaint(input, holes, output, radius, cv::INPAINT_NS);
  } catch (const cv::Exception& error) {
    if (error.code == cv::Error::StsNoMem)
      throw std::bad_alloc{};
    throw;
  }
}
}  // namespace ps::plugin_internal::inpaint_ns
