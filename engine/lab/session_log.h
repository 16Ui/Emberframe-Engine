#pragma once
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <streambuf>

namespace emberframe::lab {
// 窗口启动方式可能没有可靠控制台。保留同一份磁盘日志，不依赖终端捕获。
class SessionLog {
    class Tee final:public std::streambuf {
    public:
        Tee(std::streambuf* terminal,std::streambuf* file,std::mutex& lock):terminal_(terminal),file_(file),lock_(lock){}
    protected:
        std::streamsize xsputn(const char* data,std::streamsize size) override {
            std::lock_guard guard(lock_);const auto saved=file_->sputn(data,size);
            if(terminal_)terminal_->sputn(data,size);file_->pubsync();return saved;
        }
        int_type overflow(int_type c) override {
            if(traits_type::eq_int_type(c,traits_type::eof()))return traits_type::not_eof(c);
            const char value=traits_type::to_char_type(c);return xsputn(&value,1)==1?c:traits_type::eof();
        }
        int sync() override {
            std::lock_guard guard(lock_);if(terminal_)terminal_->pubsync();return file_->pubsync();
        }
    private:std::streambuf* terminal_;std::streambuf* file_;std::mutex& lock_;
    };
public:
    explicit SessionLog(const std::filesystem::path& path):file_(open(path)),
        old_out_(std::cout.rdbuf()),old_error_(std::cerr.rdbuf()),out_(old_out_,file_.rdbuf(),mutex_),error_(old_error_,file_.rdbuf(),mutex_){
        std::cout.rdbuf(&out_);std::cerr.rdbuf(&error_);
    }
    ~SessionLog(){std::cout.flush();std::cerr.flush();std::cout.rdbuf(old_out_);std::cerr.rdbuf(old_error_);}
    SessionLog(const SessionLog&)=delete;SessionLog& operator=(const SessionLog&)=delete;
private:
    static std::ofstream open(const std::filesystem::path& path){
        std::filesystem::create_directories(path.parent_path());std::ofstream file(path);
        if(!file)throw std::runtime_error("Cannot open workbench log: "+path.string());return file;
    }
    std::ofstream file_;std::mutex mutex_;std::streambuf* old_out_;std::streambuf* old_error_;Tee out_,error_;
};
}
