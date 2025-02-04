#pragma once
#include "processor.h"

namespace NKikimr::NBackup {

IProcessor::TPtr CreateZstdCompressingProcessor(int compressionLevel);
IProcessor::TPtr CreateZstdDecompressingProcessor(size_t bufferSize);

} // namespace NKikimr::NBackup
