#pragma once
#include <ydb/core/backup/common/checksum.h>

#include "processor.h"

namespace NKikimr::NBackup {

IProcessor::TPtr CreateChecksumProcessor(IChecksum::TPtr checksum);

} // namespace NKikimr::NBackup
