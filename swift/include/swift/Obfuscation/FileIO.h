#ifndef FileIO_h
#define FileIO_h

#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <string>

namespace swift {
namespace obfuscation {

struct MemoryBufferProvider {
    virtual ~MemoryBufferProvider() = default;
    virtual llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
    getBuffer(std::string Path) const;
};

template <typename FileType>
struct FileFactory {
    virtual ~FileFactory() = default;
    virtual llvm::ErrorOr<std::unique_ptr<FileType>> getFile(std::string Path);
};

template<typename T>
llvm::Expected<T> parseJson(std::string PathToJson,
                            const MemoryBufferProvider &BufferProvider = MemoryBufferProvider());

} //namespace obfuscation
} //namespace swift

#include "FileIO-Template.h"

#endif /* FileIO_h */
