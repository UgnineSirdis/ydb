#pragma once

#include "token.h"
#include "lexer_detail.h"

namespace NYT::NYson {

////////////////////////////////////////////////////////////////////////////////

class TStatelessLexer
{
public:
    size_t ParseToken(TStringBuf data, TToken* token);

private:
    NDetail::TLexer<TStringReader, false> Lexer_{TStringReader()};
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYson
