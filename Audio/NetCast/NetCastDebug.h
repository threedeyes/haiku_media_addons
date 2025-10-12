/*
 * Copyright 2025, Gerasim Troeglazov (3dEyes**), 3dEyes@gmail.com.
 * All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#ifndef NETCAST_DEBUG_H
#define NETCAST_DEBUG_H

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <OS.h>
#include <Locker.h>

#define DEBUG_LEVEL_ERROR   1
#define DEBUG_LEVEL_WARNING 2
#define DEBUG_LEVEL_CALL    3
#define DEBUG_LEVEL_INFO    4
#define DEBUG_LEVEL_VERBOSE 5

#ifndef DEBUG_LEVEL
	#ifdef DEBUG
		#define DEBUG_LEVEL DEBUG_LEVEL_INFO
	#else
		#define DEBUG_LEVEL 0
	#endif
#endif

#ifdef DEBUG

class NetCastLogger {
public:
	static NetCastLogger& Instance()
	{
		static NetCastLogger instance;
		return instance;
	}

	void Log(int level, const char* file, int line, const char* function,
			 const char* format, ...)
	{
		if (level > DEBUG_LEVEL)
			return;

		if (!fLocker.Lock())
			return;

		if (!fLogFile) {
			fLogFile = fopen("/var/log/netcast.log", "a");
			if (!fLogFile) {
				fLocker.Unlock();
				return;
			}
		}

		time_t now = time(NULL);
		struct tm* timeinfo = localtime(&now);
		char timestamp[32];
		strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);

		const char* levelStr = "UNKNOWN";
		switch (level) {
			case DEBUG_LEVEL_ERROR:   levelStr = "ERROR  "; break;
			case DEBUG_LEVEL_WARNING: levelStr = "WARNING"; break;
			case DEBUG_LEVEL_CALL:    levelStr = "CALL   "; break;
			case DEBUG_LEVEL_INFO:    levelStr = "INFO   "; break;
			case DEBUG_LEVEL_VERBOSE: levelStr = "VERBOSE"; break;
		}

		fprintf(fLogFile, "[%s] [%s] [%s:%d %s()] ",
				timestamp, levelStr, _BaseName(file), line, function);

		va_list args;
		va_start(args, format);
		vfprintf(fLogFile, format, args);
		va_end(args);

		fprintf(fLogFile, "\n");
		fflush(fLogFile);

		fLocker.Unlock();
	}

	~NetCastLogger()
	{
		if (fLogFile) {
			fclose(fLogFile);
			fLogFile = NULL;
		}
	}

private:
	NetCastLogger() : fLogFile(NULL) {}
	NetCastLogger(const NetCastLogger&);
	NetCastLogger& operator=(const NetCastLogger&);

	const char* _BaseName(const char* path)
	{
		const char* base = strrchr(path, '/');
		return base ? base + 1 : path;
	}

	FILE* fLogFile;
	BLocker fLocker;
};

#define TRACE(level, fmt, ...) \
	do { \
		if (level <= DEBUG_LEVEL) \
			NetCastLogger::Instance().Log(level, __FILE__, __LINE__, \
				__FUNCTION__, fmt, ##__VA_ARGS__); \
	} while (0)

#define TRACE_ERROR(fmt, ...)   TRACE(DEBUG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#define TRACE_WARNING(fmt, ...) TRACE(DEBUG_LEVEL_WARNING, fmt, ##__VA_ARGS__)
#define TRACE_CALL(fmt, ...)    TRACE(DEBUG_LEVEL_CALL, fmt, ##__VA_ARGS__)
#define TRACE_INFO(fmt, ...)    TRACE(DEBUG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#define TRACE_VERBOSE(fmt, ...) TRACE(DEBUG_LEVEL_VERBOSE, fmt, ##__VA_ARGS__)

#else

#define TRACE(level, fmt, ...) ((void)0)
#define TRACE_ERROR(fmt, ...)   ((void)0)
#define TRACE_WARNING(fmt, ...) ((void)0)
#define TRACE_CALL(fmt, ...)    ((void)0)
#define TRACE_INFO(fmt, ...)    ((void)0)
#define TRACE_VERBOSE(fmt, ...) ((void)0)

#endif

#endif
