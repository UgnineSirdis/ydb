#pragma once

#include <util/generic/ptr.h>
#include <util/generic/string.h>

namespace NKikimr::NBackup {

class IChecksum : public TSimpleRefCount<IChecksum> {
public:
    using TPtr = TIntrusivePtr<IChecksum>;

    virtual ~IChecksum() = default;

    virtual void AddData(TStringBuf data) = 0;
    virtual TString Serialize() = 0;
};

IChecksum::TPtr CreateChecksum();
TString ComputeChecksum(TStringBuf data);
TString ChecksumKey(const TString& objKey);

} // NKikimr::NBackup
