#pragma once
#include <string>

class HttpClient
{
public:
    static std::string Post(const std::string& message);
};
