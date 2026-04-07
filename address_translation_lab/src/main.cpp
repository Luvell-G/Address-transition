#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

enum Protection { READ_ONLY, READ_WRITE };
enum ReplacementPolicy { FIFO_POLICY, LRU_POLICY };

std::string protectionToString(Protection p) {
    return (p == READ_ONLY) ? "RO" : "RW";
}

std::string policyToString(ReplacementPolicy p) {
    return (p == FIFO_POLICY) ? "FIFO" : "LRU";
}

struct Page {
    int frame_number = -1;
    bool present = false;
    Protection protection = READ_WRITE;
    int last_access = 0;
};

struct Segment {
    int base_address = 0;
    int limit = 0; // total logical pages across all directories
    Protection protection = READ_WRITE;
};

struct TranslationOutcome {
    bool success = false;
    int physical_address = -1;
    int latency = 0;
    std::string message;
};

struct TimelineEntry {
    int time = 0;
    std::string description;
};

struct FrameRecord {
    bool occupied = false;
    int seg_id = -1;
    int dir_id = -1;
    int page_id = -1;
    int loaded_time = 0;
    int last_access = 0;
};

class PageTable {
public:
    std::vector<Page> pages;
    int page_size = 1000;

    PageTable() = default;

    PageTable(int numPages, int pageSize) : pages(numPages), page_size(pageSize) {}

    bool validPage(int pageNum) const {
        return pageNum >= 0 && pageNum < static_cast<int>(pages.size());
    }

    int getFrameNumber(int pageNum) const {
        if (!validPage(pageNum)) return -1;
        if (!pages[pageNum].present) return -1;
        return pages[pageNum].frame_number;
    }

    void setFrame(int pageNum, int frame, Protection prot, int accessTime) {
        if (!validPage(pageNum)) return;
        pages[pageNum].frame_number = frame;
        pages[pageNum].present = true;
        pages[pageNum].protection = prot;
        pages[pageNum].last_access = accessTime;
    }

    void clearPage(int pageNum) {
        if (!validPage(pageNum)) return;
        pages[pageNum].frame_number = -1;
        pages[pageNum].present = false;
        pages[pageNum].last_access = 0;
    }

    int pageCount() const {
        return static_cast<int>(pages.size());
    }
};

class DirectoryTable {
public:
    std::map<int, PageTable> pageTables;

    void addPageTable(int dirIndex, const PageTable& pt) {
        pageTables[dirIndex] = pt;
    }

    bool hasPageTable(int dirIndex) const {
        return pageTables.find(dirIndex) != pageTables.end();
    }

    PageTable& getPageTable(int dirIndex) {
        return pageTables.at(dirIndex);
    }

    const PageTable& getPageTableConst(int dirIndex) const {
        return pageTables.at(dirIndex);
    }
};

class PhysicalMemory {
public:
    int num_frames = 0;
    std::vector<FrameRecord> frames;
    int time = 0;
    ReplacementPolicy policy = FIFO_POLICY;

    PhysicalMemory() = default;

    PhysicalMemory(int frameCount, ReplacementPolicy p)
        : num_frames(frameCount), frames(frameCount), policy(p) {}

    void configure(int frameCount, ReplacementPolicy p) {
        num_frames = frameCount;
        frames.assign(frameCount, FrameRecord{});
        policy = p;
        time = 0;
    }

    int findFreeFrame() const {
        for (int i = 0; i < num_frames; ++i) {
            if (!frames[i].occupied) return i;
        }
        return -1;
    }

    int chooseVictimFrame() const {
        int victim = -1;
        if (num_frames <= 0) return -1;

        if (policy == FIFO_POLICY) {
            int oldest = std::numeric_limits<int>::max();
            for (int i = 0; i < num_frames; ++i) {
                if (frames[i].occupied && frames[i].loaded_time < oldest) {
                    oldest = frames[i].loaded_time;
                    victim = i;
                }
            }
        } else {
            int leastRecent = std::numeric_limits<int>::max();
            for (int i = 0; i < num_frames; ++i) {
                if (frames[i].occupied && frames[i].last_access < leastRecent) {
                    leastRecent = frames[i].last_access;
                    victim = i;
                }
            }
        }
        return victim;
    }

    void occupyFrame(int frameIndex, int seg, int dir, int page) {
        if (frameIndex < 0 || frameIndex >= num_frames) return;
        frames[frameIndex].occupied = true;
        frames[frameIndex].seg_id = seg;
        frames[frameIndex].dir_id = dir;
        frames[frameIndex].page_id = page;
        frames[frameIndex].loaded_time = time;
        frames[frameIndex].last_access = time;
    }

    void touchFrame(int frameIndex) {
        if (frameIndex < 0 || frameIndex >= num_frames) return;
        if (!frames[frameIndex].occupied) return;
        frames[frameIndex].last_access = time;
    }

    void clearFrame(int frameIndex) {
        if (frameIndex < 0 || frameIndex >= num_frames) return;
        frames[frameIndex] = FrameRecord{};
    }

    double utilization() const {
        if (num_frames == 0) return 0.0;
        int used = 0;
        for (const auto& f : frames) {
            if (f.occupied) used++;
        }
        return static_cast<double>(used) / num_frames * 100.0;
    }
};

class SegmentTable {
public:
    std::vector<Segment> segments;
    std::map<int, DirectoryTable> directoryTables;
    PhysicalMemory physMem;
    std::vector<TimelineEntry> timeline;

    int translation_count = 0;
    int success_count = 0;
    int fault_count = 0;
    int page_fault_count = 0;
    int replacement_count = 0;
    int total_latency = 0;

    std::ofstream errorLog;

    SegmentTable(int numFrames = 0, ReplacementPolicy policy = FIFO_POLICY)
        : physMem(numFrames, policy) {
        errorLog.open("errors.txt", std::ios::out);
    }

    ~SegmentTable() {
        if (errorLog.is_open()) errorLog.close();
    }

    void addSegment(int id, int base, int limit, Protection prot) {
        if (id >= static_cast<int>(segments.size())) {
            segments.resize(id + 1);
        }
        segments[id] = {base, limit, prot};
    }

    void addDirectoryTable(int segId, const DirectoryTable& dt) {
        directoryTables[segId] = dt;
    }

    bool validSegment(int segNum) const {
        return segNum >= 0 && segNum < static_cast<int>(segments.size());
    }

    void logError(const std::string& msg) {
        if (errorLog.is_open()) errorLog << msg << "\n";
    }

    void addTimeline(const std::string& msg) {
        timeline.push_back({translation_count, msg});
    }

    TranslationOutcome translateAddress(int segNum, int pageDir, int pageNum, int offset, Protection accessType) {
        TranslationOutcome out;
        translation_count++;
        physMem.time++;
        out.latency = 1 + std::rand() % 5;
        total_latency += out.latency;

        if (!validSegment(segNum)) {
            std::ostringstream oss;
            oss << "Invalid segment " << segNum << ", max "
                << (segments.empty() ? -1 : static_cast<int>(segments.size()) - 1);
            out.message = "Segmentation Fault: " + oss.str();
            fault_count++;
            logError(out.message);
            addTimeline("Time " + std::to_string(translation_count) + ": " + out.message);
            return out;
        }

        Segment& segment = segments[segNum];

        if (accessType == READ_WRITE && segment.protection == READ_ONLY) {
            out.message = "Protection Violation: Cannot write to read-only segment";
            fault_count++;
            logError(out.message);
            addTimeline("Time " + std::to_string(translation_count) + ": " + out.message);
            return out;
        }

        auto dirIt = directoryTables.find(segNum);
        if (dirIt == directoryTables.end()) {
            out.message = "Directory Fault: Missing directory table";
            fault_count++;
            logError(out.message);
            addTimeline("Time " + std::to_string(translation_count) + ": " + out.message);
            return out;
        }

        DirectoryTable& dt = dirIt->second;

        if (!dt.hasPageTable(pageDir)) {
            std::ostringstream oss;
            oss << "Invalid page directory " << pageDir << " for segment " << segNum;
            out.message = "Directory Fault: " + oss.str();
            fault_count++;
            logError(out.message);
            addTimeline("Time " + std::to_string(translation_count) + ": " + out.message);
            return out;
        }

        PageTable& pt = dt.getPageTable(pageDir);

        if (!pt.validPage(pageNum)) {
            std::ostringstream oss;
            oss << "Invalid page " << pageNum << " in directory " << pageDir;
            out.message = "Page Fault: " + oss.str();
            fault_count++;
            page_fault_count++;
            logError(out.message);
            addTimeline("Time " + std::to_string(translation_count) + ": " + out.message);
            return out;
        }

        int pagesPerDir = pt.pageCount();
        int flatPage = pageDir * pagesPerDir + pageNum;
        if (flatPage >= segment.limit) {
            std::ostringstream oss;
            oss << "Page " << flatPage << " exceeds segment limit " << segment.limit;
            out.message = "Page Fault: " + oss.str();
            fault_count++;
            page_fault_count++;
            logError(out.message);
            addTimeline("Time " + std::to_string(translation_count) + ": " + out.message);
            return out;
        }

        if (offset < 0 || offset >= pt.page_size) {
            std::ostringstream oss;
            oss << "Offset " << offset << " exceeds page size " << pt.page_size;
            out.message = "Offset Fault: " + oss.str();
            fault_count++;
            logError(out.message);
            addTimeline("Time " + std::to_string(translation_count) + ": " + out.message);
            return out;
        }

        if (accessType == READ_WRITE && pt.pages[pageNum].protection == READ_ONLY) {
            out.message = "Protection Violation: Cannot write to read-only page";
            fault_count++;
            logError(out.message);
            addTimeline("Time " + std::to_string(translation_count) + ": " + out.message);
            return out;
        }

        int frame = pt.getFrameNumber(pageNum);

        if (frame == -1) {
            page_fault_count++;

            int freeFrame = physMem.findFreeFrame();
            if (freeFrame != -1) {
                frame = freeFrame;
            } else {
                frame = physMem.chooseVictimFrame();
                replacement_count++;

                if (frame == -1) {
                    out.message = "Replacement Fault: No victim frame available";
                    fault_count++;
                    logError(out.message);
                    addTimeline("Time " + std::to_string(translation_count) + ": " + out.message);
                    return out;
                }

                FrameRecord victim = physMem.frames[frame];

                if (victim.seg_id >= 0) {
                    auto oldSegIt = directoryTables.find(victim.seg_id);
                    if (oldSegIt != directoryTables.end()) {
                        if (oldSegIt->second.hasPageTable(victim.dir_id)) {
                            PageTable& oldPT = oldSegIt->second.getPageTable(victim.dir_id);
                            oldPT.clearPage(victim.page_id);
                        }
                    }
                }
            }

            pt.setFrame(pageNum, frame, pt.pages[pageNum].protection, physMem.time);
            physMem.occupyFrame(frame, segNum, pageDir, pageNum);
        } else {
            physMem.touchFrame(frame);
            pt.pages[pageNum].last_access = physMem.time;
        }

        physMem.touchFrame(frame);

        out.success = true;
        out.physical_address = segment.base_address + frame * pt.page_size + offset;
        out.message = "Translation successful";
        success_count++;

        std::ostringstream oss;
        oss << "Time " << translation_count
            << ": S" << segNum
            << " D" << pageDir
            << " P" << pageNum
            << " -> Physical " << out.physical_address
            << " (Latency " << out.latency << ")";
        addTimeline(oss.str());

        return out;
    }

    void printMemoryMap() const {
        std::cout << "\n=== Memory Map ===\n";
        for (size_t i = 0; i < segments.size(); ++i) {
            std::cout << "Segment " << i
                      << ": Base=" << segments[i].base_address
                      << ", Limit=" << segments[i].limit
                      << ", Protection=" << protectionToString(segments[i].protection)
                      << "\n";

            auto it = directoryTables.find(static_cast<int>(i));
            if (it == directoryTables.end()) {
                std::cout << "  No directory table\n";
                continue;
            }

            for (const auto& dirPair : it->second.pageTables) {
                std::cout << "  Directory " << dirPair.first << ":\n";
                const PageTable& pt = dirPair.second;

                for (size_t j = 0; j < pt.pages.size(); ++j) {
                    const auto& p = pt.pages[j];
                    std::cout << "    Page " << j
                              << ": Frame=" << p.frame_number
                              << ", Present=" << p.present
                              << ", Protection=" << protectionToString(p.protection)
                              << ", LastAccess=" << p.last_access
                              << "\n";
                }
            }
        }

        std::cout << "\nPhysical Frames:\n";
        for (int i = 0; i < physMem.num_frames; ++i) {
            const auto& f = physMem.frames[i];
            std::cout << "  Frame " << i << ": ";
            if (!f.occupied) {
                std::cout << "FREE\n";
            } else {
                std::cout << "S" << f.seg_id
                          << " D" << f.dir_id
                          << " P" << f.page_id
                          << " | loaded=" << f.loaded_time
                          << " | lastAccess=" << f.last_access
                          << "\n";
            }
        }

        std::cout << "Physical Memory Utilization: "
                  << std::fixed << std::setprecision(2)
                  << physMem.utilization() << "%\n";
    }

    void printTimeline() const {
        std::cout << "\n--- Translation Timeline ---\n";
        for (const auto& entry : timeline) {
            std::cout << entry.description << "\n";
        }
    }

    void printMetrics() const {
        std::cout << "\n=== Metrics ===\n";
        std::cout << "Replacement Policy: " << policyToString(physMem.policy) << "\n";
        std::cout << "Total Translations: " << translation_count << "\n";
        std::cout << "Successful Translations: " << success_count << "\n";
        std::cout << "Faults: " << fault_count << "\n";
        std::cout << "Page Faults: " << page_fault_count << "\n";
        std::cout << "Replacements: " << replacement_count << "\n";

        double pageFaultRate = (translation_count == 0)
            ? 0.0
            : (static_cast<double>(page_fault_count) / translation_count) * 100.0;

        std::cout << "Page Fault Rate: "
                  << std::fixed << std::setprecision(2)
                  << pageFaultRate << "%\n";

        std::cout << "Memory Utilization: "
                  << std::fixed << std::setprecision(2)
                  << physMem.utilization() << "%\n";

        double avgLatency = (translation_count == 0)
            ? 0.0
            : static_cast<double>(total_latency) / translation_count;

        std::cout << "Average Translation Time: "
                  << std::fixed << std::setprecision(2)
                  << avgLatency << "\n";
    }
};

Protection randomProtection() {
    return (std::rand() % 2 == 0) ? READ_ONLY : READ_WRITE;
}

void randomInitialize(
    SegmentTable& st,
    int numSegments,
    int dirsPerSegment,
    int pagesPerDir,
    int pageSize
) {
    st.segments.clear();
    st.directoryTables.clear();

    for (int i = 0; i < numSegments; ++i) {
        int base = i * 10000;
        int totalPages = dirsPerSegment * pagesPerDir;
        Protection segProt = randomProtection();

        st.addSegment(i, base, totalPages, segProt);

        DirectoryTable dt;

        for (int d = 0; d < dirsPerSegment; ++d) {
            PageTable pt(pagesPerDir, pageSize);

            for (int p = 0; p < pagesPerDir; ++p) {
                pt.pages[p].protection = randomProtection();

                bool initiallyPresent = (std::rand() % 2 == 0);
                if (initiallyPresent) {
                    int freeFrame = st.physMem.findFreeFrame();
                    if (freeFrame != -1) {
                        pt.pages[p].present = true;
                        pt.pages[p].frame_number = freeFrame;
                        pt.pages[p].last_access = st.physMem.time;
                        st.physMem.occupyFrame(freeFrame, i, d, p);
                    } else {
                        pt.pages[p].present = false;
                        pt.pages[p].frame_number = -1;
                    }
                } else {
                    pt.pages[p].present = false;
                    pt.pages[p].frame_number = -1;
                }
            }

            dt.addPageTable(d, pt);
        }

        st.addDirectoryTable(i, dt);
    }
}

bool loadAddressesFromFile(
    const std::string& filename,
    std::vector<std::tuple<int, int, int, int, int>>& addresses
) {
    std::ifstream in(filename);
    if (!in.is_open()) return false;

    int seg, dir, page, offset, access;
    while (in >> seg >> dir >> page >> offset >> access) {
        addresses.push_back({seg, dir, page, offset, access});
    }
    return true;
}

void processBatchFromFile(SegmentTable& st, const std::string& filename) {
    std::vector<std::tuple<int, int, int, int, int>> batch;
    if (!loadAddressesFromFile(filename, batch)) {
        std::cout << "Could not open batch file.\n";
        return;
    }

    int localSuccess = 0;
    int localFaults = 0;

    std::cout << "\n--- Batch Processing ---\n";
    for (const auto& entry : batch) {
        int seg, dir, page, offset, access;
        std::tie(seg, dir, page, offset, access) = entry;

        TranslationOutcome result = st.translateAddress(
            seg, dir, page, offset,
            access ? READ_WRITE : READ_ONLY
        );

        if (result.success) {
            localSuccess++;
            std::cout << "Physical Address: " << result.physical_address
                      << ", Latency: " << result.latency << "\n";
        } else {
            localFaults++;
            std::cout << result.message
                      << ", Latency: " << result.latency << "\n";
        }
    }

    std::cout << "Batch Summary: Success=" << localSuccess
              << ", Faults=" << localFaults << "\n";
    std::cout << "Memory Utilization After Batch: "
              << std::fixed << std::setprecision(2)
              << st.physMem.utilization() << "%\n";
}

void generateRandomAddresses(
    SegmentTable& st,
    int num,
    double validRatio,
    int dirsPerSegment,
    int pagesPerDir,
    int pageSize,
    const std::string& logFile
) {
    std::ofstream log(logFile);
    if (!log.is_open()) {
        std::cout << "Could not write to " << logFile << "\n";
        return;
    }

    std::mt19937 gen(static_cast<unsigned>(
        std::chrono::system_clock::now().time_since_epoch().count()));
    std::uniform_real_distribution<> prob(0.0, 1.0);

    int localFaults = 0;
    int localSuccess = 0;

    for (int i = 0; i < num; ++i) {
        bool makeValid = prob(gen) < validRatio;

        int segNum, dirNum, pageNum, offset;
        Protection access = (gen() % 2) ? READ_WRITE : READ_ONLY;

        if (makeValid && !st.segments.empty()) {
            segNum = gen() % st.segments.size();
            dirNum = gen() % dirsPerSegment;
            pageNum = gen() % pagesPerDir;
            offset = gen() % pageSize;
        } else {
            segNum = gen() % (static_cast<int>(st.segments.size()) + 5);
            dirNum = gen() % (dirsPerSegment + 3);
            pageNum = gen() % (pagesPerDir + 5);
            offset = gen() % (pageSize + 500);
        }

        TranslationOutcome result = st.translateAddress(segNum, dirNum, pageNum, offset, access);

        log << "Address " << i + 1 << ": ("
            << segNum << ", " << dirNum << ", " << pageNum << ", "
            << offset << ", " << (access == READ_ONLY ? "Read" : "Write") << ") ";

        if (result.success) {
            localSuccess++;
            log << "Physical=" << result.physical_address
                << ", Latency=" << result.latency << "\n";
        } else {
            localFaults++;
            log << "Failed: " << result.message
                << ", Latency=" << result.latency << "\n";
        }
    }

    double faultRate = (num == 0) ? 0.0 : static_cast<double>(localFaults) / num * 100.0;
    log << "Summary: Success=" << localSuccess
        << ", Faults=" << localFaults
        << ", Fault Rate=" << std::fixed << std::setprecision(2) << faultRate << "%\n";

    std::cout << "Results logged to " << logFile << "\n";
    std::cout << "Memory Utilization After Stress Test: "
              << std::fixed << std::setprecision(2)
              << st.physMem.utilization() << "%\n";
}

int main() {
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    int numFrames, pageSize, numSegments, dirsPerSegment, pagesPerDir;
    int policyChoice;

    std::cout << "Enter number of physical frames: ";
    std::cin >> numFrames;
    std::cout << "Enter page size: ";
    std::cin >> pageSize;
    std::cout << "Enter number of segments: ";
    std::cin >> numSegments;
    std::cout << "Enter number of page directories per segment: ";
    std::cin >> dirsPerSegment;
    std::cout << "Enter number of pages per directory: ";
    std::cin >> pagesPerDir;
    std::cout << "Choose replacement policy (1=FIFO, 2=LRU): ";
    std::cin >> policyChoice;

    if (numFrames <= 0 || pageSize <= 0 || numSegments <= 0 || dirsPerSegment <= 0 || pagesPerDir <= 0) {
        std::cout << "Invalid configuration.\n";
        return 1;
    }

    ReplacementPolicy policy = (policyChoice == 2) ? LRU_POLICY : FIFO_POLICY;
    SegmentTable segmentTable(numFrames, policy);

    randomInitialize(segmentTable, numSegments, dirsPerSegment, pagesPerDir, pageSize);

    std::cout << "\nInitial configuration complete.\n";
    std::cout << "Replacement Policy: " << policyToString(policy) << "\n";
    std::cout << "Total logical pages: " << (numSegments * dirsPerSegment * pagesPerDir) << "\n";

    segmentTable.printMemoryMap();

    std::cout << "\nEnter logical address as: seg dir page offset access[0=read,1=write]\n";
    std::cout << "Enter -1 to stop manual input.\n";

    while (true) {
        int segNum;
        std::cout << "\nseg: ";
        std::cin >> segNum;
        if (segNum == -1) break;

        int dirNum, pageNum, offset, access;
        std::cout << "dir page offset access: ";
        std::cin >> dirNum >> pageNum >> offset >> access;

        TranslationOutcome result = segmentTable.translateAddress(
            segNum, dirNum, pageNum, offset,
            access ? READ_WRITE : READ_ONLY
        );

        if (result.success) {
            std::cout << "Physical Address: " << result.physical_address
                      << ", Latency: " << result.latency << "\n";
        } else {
            std::cout << result.message
                      << ", Latency: " << result.latency << "\n";
        }

        segmentTable.printMemoryMap();
    }

    char batchChoice;
    std::cout << "\nProcess a batch of addresses from file? (y/n): ";
    std::cin >> batchChoice;
    if (batchChoice == 'y' || batchChoice == 'Y') {
        std::string batchFile;
        std::cout << "Enter batch filename: ";
        std::cin >> batchFile;
        processBatchFromFile(segmentTable, batchFile);
    }

    char genRand;
    std::cout << "\nGenerate 100 random addresses? (y/n): ";
    std::cin >> genRand;
    if (genRand == 'y' || genRand == 'Y') {
        double validRatio;
        std::cout << "Enter valid ratio (0.0 to 1.0): ";
        std::cin >> validRatio;

        if (validRatio < 0.0) validRatio = 0.0;
        if (validRatio > 1.0) validRatio = 1.0;

        generateRandomAddresses(
            segmentTable,
            100,
            validRatio,
            dirsPerSegment,
            pagesPerDir,
            pageSize,
            "results.txt"
        );
    }

    segmentTable.printTimeline();
    segmentTable.printMetrics();

    std::cout << "\nDetailed faults logged to errors.txt\n";
    return 0;
}