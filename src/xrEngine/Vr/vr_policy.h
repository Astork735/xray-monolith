#pragma once
#include <ctype.h>
#include <string.h>
#include <string>

namespace vr
{
// Decode complete Windows argument tokens, including quoted segments and
// backslashes before quotes. A flag embedded in a path or token is not a flag.
inline bool HasArgument(const char* command, const char* argument)
{
    if (!command) return false;
    const size_t length = strlen(argument);
    while (*command)
    {
        while (isspace(static_cast<unsigned char>(*command))) ++command;
        std::string token;
        bool quoted = false;
        while (*command && (quoted || !isspace(static_cast<unsigned char>(*command))))
        {
            size_t slashes = 0;
            while (*command == '\\') { ++slashes; ++command; }
            if (*command == '"')
            {
                token.append(slashes / 2, '\\');
                if (slashes % 2) token += '"';
                else if (quoted && command[1] == '"') { token += '"'; ++command; }
                else quoted = !quoted;
                ++command;
            }
            else
            {
                token.append(slashes, '\\');
                if (*command && (quoted || !isspace(static_cast<unsigned char>(*command)))) token += *command++;
            }
        }
        if (token.size() == length)
        {
            size_t index = 0;
            while (index < length && tolower(static_cast<unsigned char>(token[index])) ==
                tolower(static_cast<unsigned char>(argument[index]))) ++index;
            if (index == length) return true;
        }
    }
    return false;
}
inline int LaunchOverride(const char* command)
{
    return HasArgument(command, "-novr") ? 0 : HasArgument(command, "-vr") ? 2 : -1;
}
}
