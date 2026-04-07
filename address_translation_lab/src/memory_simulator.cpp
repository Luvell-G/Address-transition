#include "memory_simulator.h"

// ---------- PageTable ----------

PageTable::PageTable(int numPages, int pSize) : page_size(pSize) {
    pages.resize(numPages);
}

int PageTable::getFrameNumber(int pageNum) {
    if (pageNum < 0 || pageNum >= (int)pages.size()) {
        throw std::out_of_range("Invalid page number");
    }

    if (!pages[pageNum].present) {
        return -1;
    }

    return pages[pageNum].frame;
}

void PageTable::setFrame(int pageNum, int frame, Protection prot) {
    if (pageNum < 0 || pageNum >= (int)pages.size()) {
        throw std::out_of_range("Invalid page number");
    }

    pages[pageNum].frame = frame;
    pages[pageNum].present = true;
    pages[pageNum].protection = prot;
    fifoQueue.push(pageNum);
}

int PageTable::replacePage() {
    while (!fifoQueue.empty()) {
        int victim = fifoQueue.front();
        fifoQueue.pop();

        if (victim >= 0 && victim < (int)pages.size() && pages[victim].present) {
            pages[victim].present = false;
            int oldFrame = pages[victim].frame;
            pages[victim].frame = -1;
            return oldFrame;
        }
    }

    return -1;
}

void PageTable::touchPage(int pageNum, int timeCounter) {
    if (pageNum < 0 || pageNum >= (int)pages.size()) {
        throw std::out_of_range("Invalid page number");
    }

    pages[pageNum].last_access = timeCounter;
}

int PageTable::getNumPages() const {
    return (int)pages.size();
}

bool PageTable::isPresent(int pageNum) const {
    if (pageNum < 0 || pageNum >= (int)pages.size()) {
        throw std::out_of_range("Invalid page number");
    }

    return pages[pageNum].present;
}

Protection PageTable::getProtection(int pageNum) const {
    if (pageNum < 0 || pageNum >= (int)pages.size()) {
        throw std::out_of_range("Invalid page number");
    }

    return pages[pageNum].protection;
}

PageEntry& PageTable::getPageEntry(int pageNum) {
    if (pageNum < 0 || pageNum >= (int)pages.size()) {
        throw std::out_of_range("Invalid page number");
    }

    return pages[pageNum];
}

int PageTable::countPresentPages() const {
    int count = 0;
    for (const auto& p : pages) {
        if (p.present) {
            count++;
        }
    }
    return count;
}

// ---------- DirectoryTable ----------

void DirectoryTable::addPageTable(int dirIndex, const PageTable& pt) {
    directory[dirIndex] = pt;
}

bool DirectoryTable::hasPageTable(int dirIndex) const {
    return directory.find(dirIndex) != directory.end();
}

PageTable& DirectoryTable::getPageTable(int dirIndex) {
    if (!hasPageTable(dirIndex)) {
        throw std::out_of_range("Page directory entry not found");
    }

    return directory[dirIndex];
}

const std::map<int, PageTable>& DirectoryTable::getAllPageTables() const {
    return directory;
}

// ---------- SegmentTable ----------

SegmentTable::SegmentTable()
    : physical_memory_size(0),
      next_free_frame(0),
      page_faults(0),
      replacements(0),
      translation_counter(0),
      successful_translations(0) {}

void SegmentTable::configureMemory(int physMemSize) {
    physical_memory_size = physMemSize;
}

void SegmentTable::addSegment(int segID, const Segment& seg) {
    if (segID >= (int)segments.size()) {
        segments.resize(segID + 1);
    }

    segments[segID] = seg;
}

void SegmentTable::addDirectoryTable(int segID, const DirectoryTable& dt) {
    directoryTables[segID] = dt;
}

int SegmentTable::handlePageFault(PageTable& pt, int pageNum, Protection prot) {
    page_faults++;
    int assignedFrame;

    if (next_free_frame < physical_memory_size) {
        assignedFrame = next_free_frame;
        next_free_frame++;
    } else {
        assignedFrame = pt.replacePage();
        replacements++;

        if (assignedFrame == -1) {
            throw std::runtime_error("Page replacement failed");
        }
    }

    pt.setFrame(pageNum, assignedFrame, prot);
    return assignedFrame;
}

TranslationResult SegmentTable::translateAddress(int segNum, int pageDir, int pageNum, int offset, AccessType accessType) {
    translation_counter++;

    if (segNum < 0 || segNum >= (int)segments.size()) {
        logTimeline("Time " + std::to_string(translation_counter) + ": Segmentation fault (invalid segment)");
        return TranslationResult(false, -1, "Segmentation fault: invalid segment");
    }

    Segment& seg = segments[segNum];

    if (directoryTables.find(segNum) == directoryTables.end()) {
        logTimeline("Time " + std::to_string(translation_counter) + ": Segmentation fault (missing directory)");
        return TranslationResult(false, -1, "Segmentation fault: missing directory table");
    }

    DirectoryTable& dt = directoryTables[segNum];

    if (!dt.hasPageTable(pageDir)) {
        logTimeline("Time " + std::to_string(translation_counter) + ": Segmentation fault (invalid page directory)");
        return TranslationResult(false, -1, "Segmentation fault: invalid page directory");
    }

    PageTable& pt = dt.getPageTable(pageDir);

    if (pageNum < 0 || pageNum >= pt.getNumPages()) {
        logTimeline("Time " + std::to_string(translation_counter) + ": Segmentation fault (invalid page)");
        return TranslationResult(false, -1, "Segmentation fault: invalid page");
    }

    int logicalPageIndex = pageDir * pt.getNumPages() + pageNum;
    if (logicalPageIndex >= seg.limit) {
        logTimeline("Time " + std::to_string(translation_counter) + ": Segmentation fault (segment limit exceeded)");
        return TranslationResult(false, -1, "Segmentation fault: page exceeds segment limit");
    }

    if (offset < 0 || offset >= pt.page_size) {
        logTimeline("Time " + std::to_string(translation_counter) + ": Segmentation fault (invalid offset)");
        return TranslationResult(false, -1, "Segmentation fault: invalid offset");
    }

    if (accessType == AccessType::WRITE && seg.protection == Protection::READ_ONLY) {
        logTimeline("Time " + std::to_string(translation_counter) + ": Protection violation (segment read-only)");
        return TranslationResult(false, -1, "Protection violation: segment is read-only");
    }

    int frame = pt.getFrameNumber(pageNum);
    if (frame == -1) {
        frame = handlePageFault(pt, pageNum, seg.protection);
    }

    if (accessType == AccessType::WRITE && pt.getProtection(pageNum) == Protection::READ_ONLY) {
        logTimeline("Time " + std::to_string(translation_counter) + ": Protection violation (page read-only)");
        return TranslationResult(false, -1, "Protection violation: page is read-only");
    }

    pt.touchPage(pageNum, translation_counter);

    int physicalAddress = seg.base_address + frame * pt.page_size + offset;
    successful_translations++;

    logTimeline("Time " + std::to_string(translation_counter) +
                ": Segment " + std::to_string(segNum) +
                " translated to physical address " + std::to_string(physicalAddress));

    return TranslationResult(true, physicalAddress, "Translation successful");
}

void SegmentTable::logTimeline(const std::string& text) {
    timeline.push_back({translation_counter, text});
}

void SegmentTable::printTimeline() const {
    std::cout << "\n--- Translation Timeline ---\n";
    for (const auto& entry : timeline) {
        std::cout << entry.description << "\n";
    }
}

int SegmentTable::getTranslationCount() const {
    return translation_counter;
}

int SegmentTable::getPageFaultCount() const {
    return page_faults;
}

int SegmentTable::getReplacementCount() const {
    return replacements;
}

double SegmentTable::getPageFaultRate() const {
    if (translation_counter == 0) {
        return 0.0;
    }
    return (double)page_faults / translation_counter;
}

double SegmentTable::getMemoryUtilization() const {
    if (physical_memory_size == 0) {
        return 0.0;
    }
    return (double)next_free_frame / physical_memory_size;
}

double SegmentTable::getAverageTranslationTime() const {
    if (translation_counter == 0) {
        return 0.0;
    }
    return 1.0;
}

void SegmentTable::printMetrics() const {
    std::cout << "\n--- Metrics ---\n";
    std::cout << "Total translations: " << translation_counter << "\n";
    std::cout << "Successful translations: " << successful_translations << "\n";
    std::cout << "Page faults: " << page_faults << "\n";
    std::cout << "Replacements: " << replacements << "\n";
    std::cout << "Page fault rate: " << getPageFaultRate() * 100.0 << "%\n";
    std::cout << "Memory utilization: " << getMemoryUtilization() * 100.0 << "%\n";
    std::cout << "Average translation time (simulated): " << getAverageTranslationTime() << "\n";
}