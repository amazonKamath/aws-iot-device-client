// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "FileLogger.h"
#include "../util/FileUtils.h"

#include <iostream>
#include <sys/stat.h> /* mkdir(2) */
#include <thread>

#define TIMESTAMP_BUFFER_SIZE 25

using namespace std;
using namespace Aws::Iot::DeviceClient::Logging;
using namespace Aws::Iot::DeviceClient::Util;

bool FileLogger::start(const PlainConfig &config)
{
    setLogLevel(config.logConfig.logLevel);
    if (!config.logConfig.file.empty())
    {
        logFile = config.logConfig.file;
    }

    struct stat info;
    string logFileDir = FileUtils::extractParentDirectory(logFile);
    if (stat(logFileDir.c_str(), &info) != 0)
    {
        cout << LOGGER_TAG << ": Cannot access " << logFileDir << "to write logs, attempting to create log directory"
             << endl;
        FileUtils::mkdirs(logFileDir.c_str());
        if (stat(logFileDir.c_str(), &info) != 0)
        {
            cout << LOGGER_TAG << ": Failed to create log directories necessary for file-based logging" << endl;
            return false;
        }
        else
        {
            cout << LOGGER_TAG << ": Successfully created log directory! Now logging to " << logFile << endl;
        }
    }
    else if (info.st_mode & S_IFDIR)
    {
        // Log directory already exists, nothing to do here
    }
    else
    {
        cout << LOGGER_TAG << ": Unknown condition encountered while trying to create log directory" << endl;
        return false;
    }

    outputStream = unique_ptr<ofstream>(new ofstream(logFile, std::fstream::app));
    if (!outputStream->fail())
    {
        thread log_thread(&FileLogger::run, this);
        log_thread.detach();
        return true;
    }

    cout << LOGGER_TAG << FormatMessage(": Failed to open %s for logging", logFile.c_str()) << endl;
    return false;
}

void FileLogger::writeLogMessage(unique_ptr<LogMessage> message)
{
    char time_buffer[TIMESTAMP_BUFFER_SIZE];
    LogUtil::generateTimestamp(message->getTime(), TIMESTAMP_BUFFER_SIZE, time_buffer);

    *outputStream << time_buffer << " " << LogLevelMarshaller::ToString(message->getLevel()) << " {"
                  << message->getTag() << "}: " << message->getMessage() << endl;
    outputStream->flush();
}

void FileLogger::run()
{
    unique_lock<mutex> runLock(isRunningLock);
    isRunning = true;
    runLock.unlock();

    while (!needsShutdown)
    {
        unique_ptr<LogMessage> message = logQueue->getNextLog();

        if (NULL != message)
        {
            writeLogMessage(std::move(message));
        }
    }
}

void FileLogger::queueLog(
    LogLevel level,
    const char *tag,
    std::chrono::time_point<std::chrono::system_clock> t,
    string message)
{
    logQueue.get()->addLog(unique_ptr<LogMessage>(new LogMessage(level, tag, t, message)));
}

void FileLogger::shutdown()
{
    needsShutdown = true;
    logQueue->shutdown();

    // If we've gotten here, we must be shutting down so we should dump the remaining messages and exit
    flush();

    unique_lock<mutex> runLock(isRunningLock);
    isRunning = false;
}

void FileLogger::flush()
{
    unique_lock<mutex> runLock(isRunningLock);
    if (!isRunning)
    {
        return;
    }
    runLock.unlock();

    while (logQueue->hasNextLog())
    {
        unique_ptr<LogMessage> message = logQueue->getNextLog();
        writeLogMessage(std::move(message));
    }
}