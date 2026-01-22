#pragma once

#include <chrono>
#include <iostream>
#include <string>

// main data are start time and timestamp function
// loggers also can do messages
class NBTlog
{
  public:
    size_t _duration_()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(CURRENT_TIME() - START_TIME)
            .count();
    }
    NBTlog() : START_TIME(CURRENT_TIME())
    {
    }

    std::chrono::time_point<std::chrono::system_clock> START_TIME;

    std::chrono::time_point<std::chrono::system_clock> CURRENT_TIME()
    {
        return std::chrono::system_clock::now();
    }

    inline void start()
    {
        START_TIME = CURRENT_TIME();
    }

    inline void stamp()
    {
        start();
    }

    inline void log()
    {
        std::cout << "[ dur " << _duration_() << "ms ]" << std::endl << std::endl;
    }

    inline void log(const std::string& message)
    {
        std::cout << "[ " << message << " ]" << std::endl
                  << "[ dur " << _duration_() << "ms ]" << std::endl
                  << std::endl;
    }
};
