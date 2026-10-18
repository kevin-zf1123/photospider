#include "common/process.hpp"
#include "psc/driver.hpp"
int main(int argc, char** argv) {
  return ps::tools::run_cli(argc, argv, ps::tools::psc::run);
}
