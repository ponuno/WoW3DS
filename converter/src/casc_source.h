// Opens the modern client's CASC storage (CascLib) as a FileDataID reader for DataDir (WoW Forever).
#pragma once
#include <string>
#include "conv.h"
namespace w3d {
// wowDir: the folder with .build.info (or its Data folder's parent).  wdtNames: forever_wdt_names.csv (map directory,FileDataID
// lines; map directory case-insensitive) - "" = look next to the executable / in the working directory.
bool openForever(DataDir& data, const std::string& wowDir, const std::string& wdtNames, std::string& err);
}  // namespace w3d
