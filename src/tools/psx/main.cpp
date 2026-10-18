#include "common/process.hpp"
#include "psx/driver.hpp"
int main(int argc, char** argv) {
  return ps::tools::run_cli(argc, argv, ps::tools::psx::run);
}
