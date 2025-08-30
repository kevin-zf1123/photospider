// FILE: include/cli/save_fp32_image.hpp
#pragma once
#include <string>
#include <opencv2/core.hpp>
#include "cli_config.hpp"

bool save_fp32_image(const cv::Mat& mat, const std::string& path, const CliConfig& config);

