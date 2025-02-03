#pragma once
#include "processor.h"

namespace NKikimr::NBackup {

IProcessor::TPtr CreateZstdCompressingProcessor(int compressionLevel);
IProcessor::TPtr CreateZstdDecompressingProcessor();

} // namespace NKikimr::NBackup
