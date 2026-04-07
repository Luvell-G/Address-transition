#ifndef MEMORY_SIMULATOR_H
#define MEMORY_SIMULATOR_H

#include <vector>
#include <map>
#include <queue>
#include <stdexcept>
#include <iostream>
#include <string>

enum class Protection {
    READ_ONLY,
    READ_WRITE
};

enum class AccessType {
    READ,
    WRITE
};

struct Segment {
    int base_address;
    int limit;
    Protection protection;

    Segment(int base = 0, int lim = 0, Protection prot = Protection::READ_WRITE)
        : base_address(base), limit(lim), protection(prot) {}
};

struct PageEntry {
    int frame;
    bool present;
    Protection protection;
    int last_access;

    PageEntry(int frm = -1, bool pres = false,
              Protection prot = Protection::READ_WRITE, int access = 0)
        : frame(frm), present(pres), protection(prot), last_access(access) {}
};

class PageTable {
private:
    std::vector<PageEntry> pages;
    std::queue<int> fifoQueue;

public:
    int page_size;

    PageTable(int numPages = 0, int pSize = 256);

    int getFrameNumber(int pageNum);
    void setFrame(int pageNum, int frame, Protection prot);
    int replacePage();
    void touchPage(int pageNum, int timeCounter);
    int getNumPages() const;
    bool isPresent(int pageNum) const;
    Protection getProtection(int pageNum) const;
    PageEntry& getPageEntry(int pageNum);
    int countPresentPages() const;
};

class DirectoryTable {
private:
    std::map<int, PageTable> directory;

public:
    void addPageTable(int dirIndex, const PageTable& pt);
    bool hasPageTable(int dirIndex) const;
    PageTable& getPageTable(int dirIndex);
    const std::map<int, PageTable>& getAllPageTables() const;
};

struct TranslationResult {
    bool success;
    int physicalAddress;
    std::string message;

    TranslationResult(bool ok = false, int addr = -1, const std::string& msg = "")
        : success(ok), physicalAddress(addr), message(msg) {}
};

struct TimelineEntry {
    int time;
    std::string description;
};

class SegmentTable {
private:
    int physical_memory_size;
    int next_free_frame;
    int page_faults;
    int replacements;
    int translation_counter;
    int successful_translations;

public:
    std::vector<Segment> segments;
    std::map<int, DirectoryTable> directoryTables;
    std::vector<TimelineEntry> timeline;

    SegmentTable();

    void configureMemory(int physMemSize);
    void addSegment(int segID, const Segment& seg);
    void addDirectoryTable(int segID, const DirectoryTable& dt);

    TranslationResult translateAddress(int segNum, int pageDir, int pageNum, int offset, AccessType accessType);

    int handlePageFault(PageTable& pt, int pageNum, Protection prot);

    void logTimeline(const std::string& text);
    void printTimeline() const;
    void printMetrics() const;

    int getTranslationCount() const;
    int getPageFaultCount() const;
    int getReplacementCount() const;
    double getPageFaultRate() const;
    double getMemoryUtilization() const;
    double getAverageTranslationTime() const;
};

#endif