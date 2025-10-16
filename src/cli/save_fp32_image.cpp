// FILE: src/cli/save_fp32_image.cpp
#include "cli/save_fp32_image.hpp"

#include <iostream>
#include <opencv2/imgcodecs.hpp>

bool save_fp32_image(const cv::Mat& mat, const std::string& path,
                     const CliConfig& config) {
  if (mat.empty()) {
    std::cout << "Error: Cannot save an empty image.\n";
    return false;
  }
  cv::Mat out_mat;
  if (config.cache_precision == "int16") {
    mat.convertTo(out_mat, CV_16U, 65535.0);
  } else {
    mat.convertTo(out_mat, CV_8U, 255.0);
  }
  return cv::imwrite(path, out_mat);
}
