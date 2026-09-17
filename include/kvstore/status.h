#ifndef KVSTORE_STATUS_H_
#define KVSTORE_STATUS_H_

#include <string>

namespace kvstore {

class Status {
public:
    Status() : code_(kOk), state_("") {}
    ~Status() = default;

    // Copy and assign
    Status(const Status& rhs) = default;
    Status& operator=(const Status& rhs) = default;

    static Status OK() { return Status(); }
    static Status NotFound(const std::string& msg) { return Status(kNotFound, msg); }
    static Status IOError(const std::string& msg) { return Status(kIOError, msg); }
    static Status InvalidArgument(const std::string& msg) { return Status(kInvalidArgument, msg); }

    bool ok() const { return code_ == kOk; }
    bool IsNotFound() const { return code_ == kNotFound; }
    bool IsIOError() const { return code_ == kIOError; }
    bool IsInvalidArgument() const { return code_ == kInvalidArgument; }

    std::string ToString() const {
        if (ok()) return "OK";
        std::string result;
        switch (code_) {
            case kNotFound: result = "NotFound: "; break;
            case kIOError: result = "IOError: "; break;
            case kInvalidArgument: result = "InvalidArgument: "; break;
            default: result = "Unknown code: "; break;
        }
        result.append(state_);
        return result;
    }

private:
    enum Code {
        kOk = 0,
        kNotFound = 1,
        kIOError = 2,
        kInvalidArgument = 3
    };

    Status(Code code, const std::string& msg) : code_(code), state_(msg) {}

    Code code_;
    std::string state_;
};

} // namespace kvstore

#endif // KVSTORE_STATUS_H_
