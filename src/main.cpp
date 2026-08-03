#include "luatier/lua.h"

#ifdef _MSC_VER
#include <crtdbg.h>
#include <stdlib.h>
#endif

namespace luatier {
int standalone_main(int argc, char** argv);
}

int main(int argc, char** argv) {
#ifdef _MSC_VER
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG);
#endif
  return luatier::standalone_main(argc, argv);
}
