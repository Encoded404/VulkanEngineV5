#ifndef PRIVATE_CODE_HPP
#define PRIVATE_CODE_HPP

// non-public header file, aka not installed and only used inside the library

namespace modern_cmake_template
{
    class PrivateCode
    {
    public:
        static int doSomethingPrivate(int x);
    };
} // namespace modern_cmake_template

#endif // PRIVATE_CODE_HPP