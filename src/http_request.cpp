

//有限状态机实现
#include "http_request.h"
#include "http_response.h"
#include "Logger.h"
#include <fcntl.h>
#include <sys/stat.h>





bool HttpRequest::Parse(std::string& buffer) {
    size_t end_pos = buffer.find("\r\n\r\n");
    if (end_pos == std::string::npos) return false;

    size_t first_space = buffer.find(' ');
    size_t second_space = buffer.find(' ', first_space + 1);
    
    if (first_space != std::string::npos && second_space != std::string::npos) {
        path_ = buffer.substr(first_space + 1, second_space - first_space - 1);
        if (path_ == "/") path_ = "/index.html";
        is_finished_ = true;
        return true;
    }
    return false;
}

