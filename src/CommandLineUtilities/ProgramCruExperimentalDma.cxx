/// \file ProgramCruExperimentalDma.cxx
/// \brief Based on https://gitlab.cern.ch/alice-cru/pciedma_eval
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)

#include "CommandLineUtilities/Program.h"
#include <iostream>
#include <iomanip>
#include <condition_variable>
#include <thread>
#include <queue>
#include <future>
#include <chrono>
#include <pda.h>
#include <fstream>
#include <sstream>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <ctype.h>
#include <boost/scoped_ptr.hpp>
#include <boost/format.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/circular_buffer.hpp>
#include <boost/optional.hpp>
#include "RORC/Exception.h"
#include "RorcDevice.h"
#include "MemoryMappedFile.h"
#include "Cru/CruBarAccessor.h"
#include "Cru/CruFifoTable.h"
#include "Options.h"
#include "Pda/PdaDevice.h"
#include "Pda/PdaBar.h"
#include "Pda/PdaDmaBuffer.h"
#include "Pda/Pda.h"
#include "PageAddress.h"
#include "Utilities/SmartPointer.h"
#include "Utilities/Thread.h"

/// Use busy wait instead of condition variable (c.v. impl incomplete, is very slow)
#define USE_BUSY_INTERRUPT_WAIT

namespace b = boost;
namespace bfs = boost::filesystem;
//namespace Register = AliceO2::Rorc::CruRegisterIndex;
using namespace AliceO2::Rorc::CommandLineUtilities;
using namespace AliceO2::Rorc;
using namespace std::literals;
using namespace std::chrono_literals;
using std::cout;
using std::endl;

namespace {

/// Max amount of errors that are recorded into the error stream
static constexpr int64_t MAX_RECORDED_ERRORS = 1000;

/// Determines how often the status display refreshes
constexpr auto DISPLAY_INTERVAL = 10ms;

/// DMA addresses must be 32-byte aligned
constexpr uint64_t DMA_ALIGNMENT = 32;

/// DMA page length in bytes
constexpr int DMA_PAGE_SIZE = 8 * 1024;

/// DMA page length in 32-bit words
constexpr int DMA_PAGE_SIZE_32 = DMA_PAGE_SIZE / 4;

constexpr int NUM_OF_BUFFERS = 32;
constexpr int FIFO_ENTRIES = 4;
constexpr int NUM_PAGES = FIFO_ENTRIES * NUM_OF_BUFFERS;

/// Two 2MiB hugepages. Should be enough...
constexpr size_t DMA_BUFFER_PAGES_SIZE = 4l * 1024l * 1024l;

constexpr uint32_t BUFFER_DEFAULT_VALUE = 0xCcccCccc;

/// PDA DMA buffer index for the pages buffer
constexpr int BUFFER_INDEX_PAGES = 0;

/// Timeout of SIGINT handling
constexpr auto HANDLING_SIGINT_TIMEOUT = 10ms;

/// Default number of pages
constexpr int PAGES_DEFAULT = 1500;

/// Minimum random pause interval in milliseconds
constexpr int NEXT_PAUSE_MIN = 10;
/// Maximum random pause interval in milliseconds
constexpr int NEXT_PAUSE_MAX = 2000;
/// Minimum random pause in milliseconds
constexpr int PAUSE_LENGTH_MIN = 1;
/// Maximum random pause in milliseconds
constexpr int PAUSE_LENGTH_MAX = 500;

/// The data emulator writes to every 8th 32-bit word
constexpr uint32_t PATTERN_STRIDE = 8;

/// Path of the DMA buffer shared memory file
const bfs::path DMA_BUFFER_PAGES_PATH = "/mnt/hugetlbfs/rorc-cru-experimental-dma-pages-v2";

/// Fields: Time(hour:minute:second), i, Counter, Errors, Fill, °C, GB/s, AvgPolls
const std::string PROGRESS_FORMAT_HEADER("  %-8s   %-12s  %-12s  %-10s  %-8.1f %-8s %-8s");

/// Fields: Time(hour:minute:second), i, Counter, Errors, Fill, °C, GB/s, AvgPolls
const std::string PROGRESS_FORMAT("  %02s:%02s:%02s   %-12s  %-12s  %-10s  %-8.1f %-8s %-8s");

auto READOUT_ERRORS_PATH = "readout_errors.txt";
auto READOUT_DATA_PATH_ASCII = "readout_data.txt";
auto READOUT_DATA_PATH_BIN = "readout_data.bin";
auto READOUT_LOG_FORMAT = "readout_log_%d.txt";
auto READOUT_IDLE_LOG_PATH = "readout_idle_log.txt";

namespace Stuff
{

/// Manages a temperature monitor thread
class TemperatureMonitor : public Utilities::Thread
{
  public:
    bool isValid() const
    {
      return mValidFlag.load();
    }

    bool isMaxExceeded() const
    {
      return mMaxExceeded.load();
    }

    double getTemperature() const
    {
      return mTemperature.load();
    }

    void start(volatile uint32_t* bar)
    {
      Thread::start([&](std::atomic<bool>* stopFlag) {
        while (!stopFlag->load() && !Program::isSigInt()) {
//          uint32_t value = bar[CruRegisterIndex::TEMPERATURE];
          b::optional<double> temperature = boost::none;//Cru::Temperature::convertRegisterValue(value);
          if (!temperature) {
            mValidFlag = false;
          } else {
            mValidFlag = true;
            mTemperature = *temperature;
            if (*temperature > 80.0) {
              mMaxExceeded = true;
              cout << "\n!!! MAXIMUM TEMPERATURE WAS EXCEEDED: " << *temperature << endl;
              break;
            }
          }
          std::this_thread::sleep_for(50ms);
        }
      });
    }

  private:
    /// Flag to indicate max temperature was exceeded
    std::atomic<bool> mMaxExceeded;

    /// Variable to communicate temperature value
    std::atomic<double> mTemperature;

    /// Flag for valid temperature value
    std::atomic<bool> mValidFlag;
};


class RegisterHammer : public Utilities::Thread
{
  public:
    /// Start monitoring
    /// \param temperatureRegister The register to read the temperature data from.
    ///   The object using this must guarantee it will be accessible until stop() is called or this object is destroyed.
    void start(volatile uint32_t* bar)
    {
      Thread::start([&bar](std::atomic<bool>* stopFlag){
        auto& reg = bar[CruRegisterIndex::DEBUG_READ_WRITE];
        while (!stopFlag->load() && !Program::isSigInt()) {
          for (uint32_t hostCounter = 0; hostCounter < 256; ++hostCounter) {
            reg = hostCounter;
            // std::this_thread::sleep_for(1ms);
            uint32_t regValue = reg;
            uint32_t pciCounter = regValue & 0xff;
            if (pciCounter != hostCounter) {
              cout << boost::format("REGISTER HAMMER: value: 0x%02x, expected: 0x%02x, raw: 0x%08x\n")
                  % pciCounter % hostCounter % regValue;
            }
          }
        }
      });
    }

  private:
    Thread mThread;
};

template <typename T>
struct AddressSpaces
{
    AddressSpaces() : user(nullptr), bus(nullptr)
    {
    }

    AddressSpaces(void* user, void* bus) : user(reinterpret_cast<T*>(user)), bus(reinterpret_cast<T*>(bus))
    {
    }

    T* user;
    T* bus;
};

bool checkAlignment(void* address, uint64_t alignment)
{
  return (uint64_t(address) % alignment) == 0;
}
} // namespace stuff

class ProgramCruExperimentalDma: public Program
{
  public:

    virtual Description getDescription() override
    {
      return { "CRU EXPERIMENTAL DMA", "!!! USE WITH CAUTION !!!", "./rorc-cru-experimental-dma" };
    }

    virtual void addOptions(boost::program_options::options_description& options) override
    {
      namespace po = boost::program_options;
      options.add_options()
          ("reset",
              po::bool_switch(&mOptions.resetCard),
              "Reset card during initialization")
          ("to-file-ascii",
              po::bool_switch(&mOptions.fileOutputAscii),
              "Read out to file in ASCII format")
          ("to-file-bin",
              po::bool_switch(&mOptions.fileOutputBin),
              "Read out to file in binary format (only contains raw data from pages)")
          ("pages",
              po::value<int64_t>(&mOptions.maxPages)->default_value(PAGES_DEFAULT),
              "Amount of pages to transfer. Give <= 0 for infinite.")
          ("show-fifo",
              po::bool_switch(&mOptions.fifoDisplay),
              "Display FIFO status (wide terminal recommended)")
          ("rand-pause-sw",
              po::bool_switch(&mOptions.randomPauseSoft),
              "Randomly pause readout using software method")
          ("rand-pause-fw",
              po::bool_switch(&mOptions.randomPauseFirm),
              "Randomly pause readout using firmware method")
          ("check-pattern",
              po::value<std::string>(&mOptions.generatorPatternString),
              "Error check with given pattern [INCREMENTAL, ALTERNATING, CONSTANT]")
          ("rm-sharedmem",
              po::bool_switch(&mOptions.removeSharedMemory),
              "Remove shared memory after DMA transfer")
          ("reload-kmod",
              po::bool_switch(&mOptions.reloadKernelModule),
              "Reload kernel module before DMA initialization")
          ("resync-counter",
              po::bool_switch(&mOptions.resyncCounter),
              "Automatically resynchronize data generator counter in case of errors")
          ("reg-hammer",
              po::bool_switch(&mOptions.registerHammer),
              "Stress-test the debug register with repeated writes/reads")
          ("no-200",
              po::bool_switch(&mOptions.noTwoHundred),
              "Disable writing ready status to 0x200")
          ("legacy-ack",
              po::bool_switch(&mOptions.legacyAck),
              "Legacy option: give ack every 4 pages instead of every 1 page")
          ("cumulative-idle",
              po::bool_switch(&mOptions.cumulativeIdle),
              "Calculate cumulative idle count")
          ("log-idle",
              po::bool_switch(&mOptions.logIdle),
              "Log idle counter");
        AliceO2::Rorc::CommandLineUtilities::Options::addOptionCardId(options);

    }

    virtual void run(const boost::program_options::variables_map& variablesMap) override
    {
      using namespace AliceO2::Rorc;

      if (!mOptions.generatorPatternString.empty()) {
        mOptions.checkError = true;
        mOptions.generatorPattern = GeneratorPattern::fromString(mOptions.generatorPatternString);
      } else {
        mOptions.checkError = false;
      }

      mOptions.cardId = AliceO2::Rorc::CommandLineUtilities::Options::getOptionCardId(variablesMap);

      if (mOptions.fileOutputAscii && mOptions.fileOutputBin) {
        BOOST_THROW_EXCEPTION(CruException()
            << ErrorInfo::Message("File output can't be both ASCII and binary"));
      }
      if (mOptions.fileOutputAscii) {
        mReadoutStream.open(READOUT_DATA_PATH_ASCII);
      }
      if (mOptions.fileOutputBin) {
        mReadoutStream.open(READOUT_DATA_PATH_BIN, std::ios::binary);
      }

      mInfinitePages = (mOptions.maxPages <= 0);

      auto time = std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      auto filename = b::str(b::format(READOUT_LOG_FORMAT) % time);
      mLogStream.open(filename, std::ios_base::out);
      mLogStream << "# Time " << time << "\n";

      if (mOptions.logIdle) {
        mIdleLogStream.open(READOUT_IDLE_LOG_PATH);
      }

      cout << "Initializing" << endl;
      initDma();

      cout << "Starting temperature monitor" << endl;
      mTemperatureMonitor.start(&bar(0));

      if (mOptions.registerHammer) {
        mRegisterHammer.start(&bar(0));
      }

      cout << "Starting DMA test" << endl;
      runDma();

      mTemperatureMonitor.join();
      mRegisterHammer.join();

      if (mOptions.removeSharedMemory) {
        cout << "Removing shared memory file\n";
        removeDmaBufferFile();
      }
    }

  private:
    struct Handle
    {
        int descriptorIndex; ///< Index for CRU DMA descriptor table
        int pageIndex; ///< Index for mPageAddresses
    };

    /// Underlying buffer for the ReadoutQueue
    using QueueBuffer = boost::circular_buffer<Handle>;

    /// Queue for readout page handles
    using ReadoutQueue = std::queue<Handle, QueueBuffer>;

    using TimePoint = std::chrono::high_resolution_clock::time_point;

    /// Array the size of a page
    using PageBuffer = std::array<uint32_t, DMA_PAGE_SIZE_32>;

    /// Array of pages
    using PageBufferArray = std::array<PageBuffer, NUM_PAGES>;

    void initDma()
    {
      if (mOptions.reloadKernelModule) {
        system("modprobe -r uio_pci_dma");
        system("modprobe uio_pci_dma");
      }

      initPda();
      initFifo();
      resetBuffer();
      resetCard();
      resetTemperatureSensor();
      printSomeInfo();

      initCard();
    }

    void runDma()
    {
      // Get started
      if (isVerbose()) {
        printStatusHeader();
      }
      mRunTime.start = std::chrono::high_resolution_clock::now();
      mIntervalMeasurements.reset();

      // Set first round of pages, and inform the firmware we're ready to receive
      fillReadoutQueue();
      BufferReadyGuard bufferReadyGuard {mPdaBar.get()};

      while (true) {
        // Check if we need to stop in the case of a page limit
        if (!mInfinitePages && mReadoutCounter >= mOptions.maxPages) {
          cout << "\n\nMaximum amount of pages reached\n";
          break;
        }

        // The loop break may be set because of interrupts, max temperature, etc.
        if (mDmaLoopBreak) {
          break;
        }

        // Note: these low priority tasks are not run on every cycle, to reduce overhead
        lowPriorityTasks();

        // Keep the readout queue filled
        fillReadoutQueue();

        // Read out a page if available
        if (readoutQueueHasPageAvailable()) {
          readoutPage(mQueue.front());

          // Indicate to the firmware we've read out the page
          if (mOptions.legacyAck) {
            if (mReadoutCounter % 4 == 0) {
              acknowledgePage();
            }
          }
          else {
            acknowledgePage();
          }

          mQueue.pop();
        }
      }

      // Finish up
      mIdleCountLower32 = getBar().getIdleCounterLower();
      mIdleCountUpper32 = getBar().getIdleCounterUpper();
      mIdleMaxValue = getBar().getIdleMaxValue();
      mRunTime.end = std::chrono::high_resolution_clock::now();
      outputErrors();
      outputStats();
    }

    void acknowledgePage()
    {
      getBar().sendAcknowledge();

      if (mOptions.cumulativeIdle || mOptions.logIdle) {
        uint64_t idle = getBar().getIdleCounter();

        if (mOptions.cumulativeIdle) {
          mIdleCountCumulative += idle;
        }

        if (mOptions.logIdle) {
          auto time = std::chrono::high_resolution_clock::now() - mRunTime.start;
          auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(time).count();
          mIdleLogStream << nanos << " " << idle << '\n';
        }
      }
    }

    void readoutPage(const Handle& handle)
    {
      // Read out to file
      if (mOptions.fileOutputAscii || mOptions.fileOutputBin) {
        printToFile(handle, mReadoutCounter);
      }

      // Data error checking
      if (mOptions.checkError) {
        if (mDataGeneratorCounter == -1) {
          // First page initializes the counter
          mDataGeneratorCounter = getPageAddress(handle)[0];
        }

        bool hasError = checkErrors(mOptions.generatorPattern, handle, mReadoutCounter, mDataGeneratorCounter);
        if (hasError && mOptions.resyncCounter) {
          // Resync the counter
          mDataGeneratorCounter = getPageAddress(handle)[0];
        }
      }

      // Setting the buffer to the default value after the readout
      resetPage(getPageAddress(handle));

      // Reset status entry
      mFifoAddress.user->statusEntries[handle.descriptorIndex].reset();

      mDataGeneratorCounter += 256;
      mReadoutCounter++;
    }

    void initCard()
    {
      // Status base address in the bus address space
      if (Utilities::getUpper32Bits(uint64_t(mFifoAddress.bus)) != 0) {
        cout << "Warning: using 64-bit region for status bus address (" << reinterpret_cast<void*>(mFifoAddress.bus)
            << "), may be unsupported by PCI/BIOS configuration.\n";
      } else {
        cout << "Info: using 32-bit region for status bus address (" << reinterpret_cast<void*>(mFifoAddress.bus) << ")\n";
      }
      cout << "Info: status user address (" << reinterpret_cast<void*>(mFifoAddress.user) << ")\n";

      if (!Stuff::checkAlignment(mFifoAddress.bus, DMA_ALIGNMENT)) {
        BOOST_THROW_EXCEPTION(CruException() << ErrorInfo::Message("mFifoDevice not 32 byte aligned"));
      }

      getBar().setFifoBusAddress(mFifoAddress.bus);

      // TODO Note: this stuff may be set by firmware in the future
      {
        // Status base address in the card's address space
        getBar().setFifoCardAddress();
        // Set descriptor table size (must be size - 1)
        getBar().setDescriptorTableSize();
        // Send command to the DMA engine to write to every status entry, not just the final one
        getBar().setDoneControl();
      }

      if (mOptions.checkError) {
        getBar().setDataGeneratorPattern(mOptions.generatorPattern);
      }
    }

    /// Initializes PDA objects and accompanying shared memory files
    void initPda()
    {
      Utilities::resetSmartPtr(mRorcDevice, mOptions.cardId);
      Utilities::resetSmartPtr(mPdaBar, mRorcDevice->getPciDevice(), mChannelNumber);
      Utilities::resetSmartPtr(mMappedFilePages, DMA_BUFFER_PAGES_PATH.c_str(), DMA_BUFFER_PAGES_SIZE);
      Utilities::resetSmartPtr(mBufferPages, mRorcDevice->getPciDevice(), mMappedFilePages->getAddress(),
          mMappedFilePages->getSize(), BUFFER_INDEX_PAGES);
    }

    /// Initializes the FIFO and the page addresses for it
    void initFifo()
    {
      /// Amount of space reserved for the FIFO, we use multiples of the page size for uniformity
      size_t fifoSpace = ((sizeof(CruFifoTable) / DMA_PAGE_SIZE) + 1) * DMA_PAGE_SIZE;

      PageAddress fifoAddress;
      std::tie(fifoAddress, mPageAddresses) = Pda::partitionScatterGatherList(mBufferPages->getScatterGatherList(),
          fifoSpace, DMA_PAGE_SIZE);
      mFifoAddress.user = reinterpret_cast<CruFifoTable*>(const_cast<void*>(fifoAddress.user));
      mFifoAddress.bus = reinterpret_cast<CruFifoTable*>(const_cast<void*>(fifoAddress.bus));

      if (mPageAddresses.size() <= NUM_PAGES) {
        BOOST_THROW_EXCEPTION(CrorcException()
            << ErrorInfo::Message("Insufficient amount of pages fit in DMA buffer"));
      }

      // Initializing the descriptor table
      mFifoAddress.user->resetStatusEntries();

      // As a safety measure, we put "valid" addresses in the descriptor table, even though we're not pushing pages yet
      // This helps prevent the card from writing to invalid addresses and crashing absolutely everything
      for (int i = 0; i < mFifoAddress.user->descriptorEntries.size(); i++) {
        setDescriptor(i, i);
      }
    }

    void resetBuffer()
    {
      for (auto& page : mPageAddresses) {
        resetPage(page.user);
      }
    }

    void resetCard()
    {
      if (mOptions.resetCard) {
        cout << "Resetting..." << std::flush;

        getBar().resetDataGeneratorCounter();
        std::this_thread::sleep_for(100ms);
        getBar().resetCard();
        std::this_thread::sleep_for(100ms);
        cout << "done!" << endl;
      }
    }

    void resetTemperatureSensor()
    {
//      bar(Register::TEMPERATURE) = 0x1;
//      std::this_thread::sleep_for(10ms);

      //      bar(Register::TEMPERATURE) = 0x0;
//      std::this_thread::sleep_for(10ms);
//      bar(Register::TEMPERATURE) = 0x2;
//      std::this_thread::sleep_for(10ms);
    }

    void setDescriptor(int pageIndex, int descriptorIndex)
    {
      auto& pageAddress = mPageAddresses.at(pageIndex);
      auto sourceAddress = reinterpret_cast<volatile void*>((descriptorIndex % NUM_OF_BUFFERS) * DMA_PAGE_SIZE);
      mFifoAddress.user->setDescriptor(descriptorIndex, DMA_PAGE_SIZE_32, sourceAddress, pageAddress.bus);
    }

    void printSomeInfo()
    {
      if (isVerbose()) {
        mRorcDevice->printDeviceInfo(cout);
      }

      auto firmwareVersion = CommandLineUtilities::Common::make32hexString(getBar().getFirmwareCompileInfo());
//      auto serialNumber = Utilities::Common::make32hexString(bar(Register::SERIAL_NUMBER));
      cout << "  Firmware version  " << firmwareVersion << '\n';
//      cout << "  Serial number     " << serialNumber << '\n';
      cout << "  Buffer size       " << mPageAddresses.size() << " pages, " << " "
          << mPageAddresses.size() * DMA_BUFFER_PAGES_SIZE << " bytes\n";

      mLogStream << "# Firmware version  " << firmwareVersion << '\n';
//      mLogStream << "# Serial number     " << serialNumber << '\n';
      mLogStream << "# Buffer size       " << mPageAddresses.size() << " pages, " << " "
          << mPageAddresses.size() * DMA_BUFFER_PAGES_SIZE << " bytes\n";
    }

    void updateStatusDisplay()
    {
      auto format = b::format(PROGRESS_FORMAT);

      using namespace std::chrono;
      auto diff = high_resolution_clock::now() - mRunTime.start;
      auto second = duration_cast<seconds>(diff).count() % 60;
      auto minute = duration_cast<minutes>(diff).count() % 60;
      auto hour = duration_cast<hours>(diff).count();
      format % hour % minute % second;

      format % mReadoutCounter;

      mOptions.checkError ? format % mErrorCount : format % "n/a";

      format % mLastFillSize;

      mTemperatureMonitor.isValid() ? format % mTemperatureMonitor.getTemperature() : format % "n/a";

      {
        double seconds = mIntervalMeasurements.getSecondsSinceStart();
        if (seconds > 0.1) {
        double bytes = double(mIntervalMeasurements.pages) * DMA_PAGE_SIZE;
        double GB = bytes / (1000 * 1000 * 1000);
        double GBs = GB / seconds;
        format % GBs;

        double polls = mIntervalMeasurements.polls;
        double AvgPolls = polls / seconds;
        format % AvgPolls;
        } else {
          format % '-' % '-';
        }
      }

      cout << '\r' << format;

      if (mOptions.fifoDisplay) {
        char separator = '|';
        char waiting   = 'O';
        char arrived   = 'X';
        char available = ' ';

        for (int i = 0; i < 128; ++i) {
          if ((i % 8) == 0) {
            cout << separator;
          }

          cout << (i == mQueue.front().descriptorIndex) ? waiting  // We're waiting on this page
              : mFifoAddress.user->statusEntries.at(i).isPageArrived() ? arrived // This page has arrived
              : available; // This page is clear/available
        }
        cout << separator;
      }

      // This takes care of adding a "line" to the stdout and log table every so many seconds
      {
        int interval = 60;
        auto second = duration_cast<seconds>(diff).count() % interval;
        if (mDisplayUpdateNewline && second == 0) {
          cout << '\n';
          mLogStream << '\n' << format;
          mDisplayUpdateNewline = false;
          mIntervalMeasurements.reset();
        }
        if (second >= 1) {
          mDisplayUpdateNewline = true;
        }
      }
    }

    void printStatusHeader()
    {
      auto line1 = b::format(PROGRESS_FORMAT_HEADER) % "Time" % "Pages" % "Errors" % "Fill" % "°C" % "GB/s"
          % "AvgPolls";
      auto line2 = b::format(PROGRESS_FORMAT) % "00" % "00" % "00" % '-' % '-' % '-' % '-' % '-' % '-';
      cout << '\n' << line1;
      cout << '\n' << line2;
      mLogStream << '\n' << line1;
      mLogStream << '\n' << line2;
    }

    bool isStatusDisplayInterval()
    {
      auto now = std::chrono::high_resolution_clock::now();
      if (std::chrono::duration_cast<std::chrono::milliseconds, int64_t>(now - mLastDisplayUpdate) > DISPLAY_INTERVAL) {
        mLastDisplayUpdate = now;
        return true;
      }
      return false;
    }

    bool isPageArrived(const Handle& handle)
    {
      return mFifoAddress.user->statusEntries[handle.descriptorIndex].isPageArrived();
    }

    volatile uint32_t* getPageAddress(const Handle& handle)
    {
      return reinterpret_cast<volatile uint32_t*>(mPageAddresses[handle.pageIndex].user);
    }

    volatile uint32_t& bar(size_t index)
    {
//      return mPdaBar->at<uint32_t>(index);
      return mPdaBar->getUserspaceAddressU32()[index];
    }

    void lowPriorityTasks()
    {
      // This stuff doesn't need to be run every cycle, so we reduce the overhead.
      if (mLowPriorityCounter < LOW_PRIORITY_INTERVAL) {
        mLowPriorityCounter++;
        return;
      }
      mLowPriorityCounter = 0;

      // Handle a max temperature abort
      if (mTemperatureMonitor.isMaxExceeded()) {
        cout << "\n\n!!! ABORTING: MAX TEMPERATURE EXCEEDED\n";
        mDmaLoopBreak = true;
        return;
      }

      // Handle a SIGINT abort
      if (isSigInt()) {
        // We want to finish the readout cleanly if possible, so we stop pushing and try to wait a bit until the
        // queue is empty
        if (!mHandlingSigint) {
          mHandlingSigintStart = std::chrono::high_resolution_clock::now();
          mHandlingSigint = true;
          mPushEnabled = false;
        }

        if (mQueue.size() == 0) {
          // Finished readout cleanly
          cout << "\n\nInterrupted\n";
          mDmaLoopBreak = true;
          return;
        }

        if ((std::chrono::high_resolution_clock::now() - mHandlingSigintStart) > HANDLING_SIGINT_TIMEOUT) {
          // Timed out
          cout << "\n\nInterrupted (did not finish readout queue)\n";
          mDmaLoopBreak = true;
          return;
        }
      }

      // Status display updates
      if (isVerbose() && isStatusDisplayInterval()) {
        updateStatusDisplay();
      }

      // Random pauses in software: a thread sleep
      if (mOptions.randomPauseSoft) {
        auto now = std::chrono::high_resolution_clock::now();
        if (now >= mRandomPausesSoft.next) {
          cout << b::format("sw pause %-4d ms\n") % mRandomPausesSoft.length.count() << std::flush;
          std::this_thread::sleep_for(mRandomPausesSoft.length);

          // Schedule next pause
          auto now = std::chrono::high_resolution_clock::now();
          mRandomPausesSoft.next = now + std::chrono::milliseconds(Utilities::getRandRange(NEXT_PAUSE_MIN, NEXT_PAUSE_MAX));
          mRandomPausesSoft.length = std::chrono::milliseconds(Utilities::getRandRange(PAUSE_LENGTH_MIN, PAUSE_LENGTH_MAX));
        }
      }

      // Random pauses in hardware: pause the data emulator
      if (mOptions.randomPauseFirm) {
        auto now = std::chrono::high_resolution_clock::now();
        if (!mRandomPausesFirm.isPaused && now >= mRandomPausesFirm.next) {
          cout << b::format("fw pause %-4d ms\n") % mRandomPausesFirm.length.count() << std::flush;
          bar(CruRegisterIndex::DATA_EMULATOR_CONTROL) = 0x1;
          mRandomPausesFirm.isPaused = true;
        }

        if (mRandomPausesFirm.isPaused && now >= mRandomPausesFirm.next + mRandomPausesFirm.length) {
          bar(CruRegisterIndex::DATA_EMULATOR_CONTROL) = 0x3;
          mRandomPausesFirm.isPaused = false;

          // Schedule next pause
          auto now = std::chrono::high_resolution_clock::now();
          mRandomPausesFirm.next = now + std::chrono::milliseconds(Utilities::getRandRange(NEXT_PAUSE_MIN, NEXT_PAUSE_MAX));
          mRandomPausesFirm.length = std::chrono::milliseconds(Utilities::getRandRange(PAUSE_LENGTH_MIN, PAUSE_LENGTH_MAX));
        }
      }
    }

    void removeDmaBufferFile()
    {
      bfs::remove(DMA_BUFFER_PAGES_PATH);
    }

    bool shouldPushQueue()
    {
      return (mQueue.size() < NUM_PAGES) && (mInfinitePages || (mPushCounter < mOptions.maxPages)) && mPushEnabled;
    }

    void pushPage()
    {
      // Push page
      setDescriptor(mPageIndexCounter, mDescriptorCounter);

      // Add the page to the readout queue
      mQueue.push(Handle{mDescriptorCounter, mPageIndexCounter});

      // Increment counters
      mDescriptorCounter = (mDescriptorCounter + 1) % NUM_PAGES;
      mPageIndexCounter = (mPageIndexCounter + 1) % mPageAddresses.size();
      mPushCounter++;
    }

    void fillReadoutQueue()
    {
      int pushed = 0;

      while (shouldPushQueue()) {
        pushPage();
        pushed++;
      }

      if (pushed) {
        mLastFillSize = pushed;
      }
    }

    bool readoutQueueHasPageAvailable()
    {
      return !mQueue.empty() && isPageArrived(mQueue.front());
    }

    GeneratorPattern::type getCurrentGeneratorPattern()
    {
      // Get first 2 bits of DMA configuration register, these contain the generator pattern
      uint32_t dmaConfiguration = bar(CruRegisterIndex::DMA_CONFIGURATION) && 0b11;
      return dmaConfiguration == 0b01 ? GeneratorPattern::Incremental
           : dmaConfiguration == 0b10 ? GeneratorPattern::Alternating
           : dmaConfiguration == 0b11 ? GeneratorPattern::Constant
           : GeneratorPattern::Unknown;
    }

    void outputStats()
    {
      // Calculating throughput
      double runTime = std::chrono::duration<double>(mRunTime.end - mRunTime.start).count();
      double bytes = double(mReadoutCounter) * DMA_PAGE_SIZE;
      double GB = bytes / (1000 * 1000 * 1000);
      double GBs = GB / runTime;
      double Gbs = GBs * 8.0;
      double GiB = bytes / (1024 * 1024 * 1024);
      double GiBs = GiB / runTime;
      double Gibs = GiBs * 8.0;

      auto format = b::format("  %-10s  %-10s\n");
      auto formatHex = b::format("  %-10s  0x%-10x\n");
      std::ostringstream stream;
      stream << '\n';
      stream << format % "Seconds" % runTime;
      stream << format % "Pages" % mReadoutCounter;
      if (bytes > 0.00001) {
        stream << format % "Bytes" % bytes;
        stream << format % "GB" % GB;
        stream << format % "GB/s" % GBs;
        stream << format % "Gb/s" % Gbs;
        stream << format % "GiB" % GiB;
        stream << format % "GiB/s" % GiBs;
        stream << format % "Gibit/s" % Gibs;
        stream << format % "Errors" % mErrorCount;
      }
      if (mOptions.cumulativeIdle) {
        stream << format % "Idle" % mIdleCountCumulative;
      }
      stream << formatHex % "idle_cnt lower" % mIdleCountLower32;
      stream << formatHex % "idle_cnt upper" % mIdleCountUpper32;
      stream << formatHex % "max_idle_value" % mIdleMaxValue;
      stream << '\n';

      auto str = stream.str();
      cout << str;
      mLogStream << '\n' << str;
    }

    void copyPage (uint32_t* target, const uint32_t* source)
    {
      std::copy(source, source + DMA_PAGE_SIZE_32, target);
    }

    void printToFile(const Handle& handle, int64_t pageNumber)
    {
      volatile uint32_t* page = getPageAddress(handle);

      if (mOptions.fileOutputAscii) {
        mReadoutStream << "Event #" << pageNumber << " Buffer #" << handle.pageIndex << '\n';
        int perLine = 8;
        for (int i = 0; i < DMA_PAGE_SIZE_32; i+= perLine) {
          for (int j = 0; j < perLine; ++j) {
            mReadoutStream << page[i+j] << ' ';
          }
          mReadoutStream << '\n';
        }
        mReadoutStream << '\n';
      }
      else if (mOptions.fileOutputBin) {
        // TODO Is there a more elegant way to write from volatile memory?
        mReadoutStream.write(reinterpret_cast<char*>(const_cast<uint32_t*>(page)), DMA_PAGE_SIZE);
      }
    }

    /// Checks and reports errors
    bool checkErrors(GeneratorPattern::type pattern, const Handle& handle, int64_t eventNumber, uint32_t counter)
    {
      auto check = [&](auto patternFunction) {
        volatile uint32_t* page = getPageAddress(handle);
        for (uint32_t i = 0; i < DMA_PAGE_SIZE_32; i += PATTERN_STRIDE)
        {
          uint32_t expectedValue = patternFunction(i);
          uint32_t actualValue = page[i];
          if (actualValue != expectedValue) {
            // Report error
            mErrorCount++;
            if (isVerbose() && mErrorCount < MAX_RECORDED_ERRORS) {
              mErrorStream << "Error @ event:" << eventNumber << " page:" << handle.pageIndex << " i:" << i << " exp:"
                  << expectedValue << " val:" << actualValue << '\n';
            }
            return true;
          }
        }
        return false;
      };

      switch (pattern) {
        case GeneratorPattern::Incremental: return check([&](uint32_t i) { return counter + i / 8; });
        case GeneratorPattern::Alternating: return check([&](uint32_t)   { return 0xa5a5a5a5; });
        case GeneratorPattern::Constant:    return check([&](uint32_t)   { return 0x12345678; });
        default: ;
      }

      BOOST_THROW_EXCEPTION(CruException()
          << ErrorInfo::Message("Unrecognized generator pattern")
          << ErrorInfo::GeneratorPattern(pattern));
    }

    void outputErrors()
    {
      auto errorStr = mErrorStream.str();

      if (isVerbose()) {
        size_t maxChars = 2000;
        if (!errorStr.empty()) {
          cout << "Errors:\n";
          cout << errorStr.substr(0, maxChars);
          if (errorStr.length() > maxChars) {
            cout << "\n... more follow (" << (errorStr.length() - maxChars) << " characters)\n";
          }
        }
      }

      std::ofstream stream(READOUT_ERRORS_PATH);
      stream << errorStr;
    }

    void resetPage(volatile uint32_t* page)
    {
      for (size_t i = 0; i < DMA_PAGE_SIZE_32; i++) {
        page[i] = BUFFER_DEFAULT_VALUE;
      }
    }

    void resetPage(volatile void* page)
    {
      resetPage(reinterpret_cast<volatile uint32_t*>(page));
    }

    CruBarAccessor getBar()
    {
      return CruBarAccessor(mPdaBar.get());
    }

    /// Program options
    struct Options {
        AliceO2::Rorc::Parameters::CardIdType cardId;
        int64_t maxPages = 0; ///< Limit of pages to push
        bool fileOutputAscii;
        bool fileOutputBin;
        bool resetCard;
        bool fifoDisplay;
        bool randomPauseSoft;
        bool randomPauseFirm;
        bool removeSharedMemory;
        bool reloadKernelModule;
        bool resyncCounter;
        bool registerHammer;
        bool legacyAck;
        bool noTwoHundred;
        bool logIdle;
        bool cumulativeIdle;
        std::string generatorPatternString;
        GeneratorPattern::type generatorPattern;
        bool checkError;
    } mOptions;

    /// A value of true means no limit on page pushing
    bool mInfinitePages;

    struct RunTime
    {
        TimePoint start; ///< Start of run time
        TimePoint end; ///< End of run time
    } mRunTime;

    class BufferReadyGuard
    {
      public:
        BufferReadyGuard(Pda::PdaBar* bar) : mBar(bar)
        {
          setStatus(true);
        }

        ~BufferReadyGuard()
        {
          setStatus(false);
        }

      private:
        void setStatus(bool ready)
        {
          CruBarAccessor(mBar).setDataEmulatorEnabled(ready);
        }

        Pda::PdaBar* mBar;
    };

    /// Temperature monitor thread thing
    Stuff::TemperatureMonitor mTemperatureMonitor;

    /// Register hammer
    Stuff::RegisterHammer mRegisterHammer;

    // PDA, buffer, etc stuff
    b::scoped_ptr<RorcDevice> mRorcDevice;
    b::scoped_ptr<Pda::PdaBar> mPdaBar;
    b::scoped_ptr<MemoryMappedFile> mMappedFilePages;
    b::scoped_ptr<Pda::PdaDmaBuffer> mBufferPages;

    /// Aliased userspace FIFO
    Stuff::AddressSpaces<CruFifoTable> mFifoAddress;

    /// Amount of pages pushed
    int64_t mPushCounter = 0;

    /// Amount of pages read out
    int64_t mReadoutCounter = 0;

    /// Data generator counter
    int64_t mDataGeneratorCounter = -1;

    /// Indicates current descriptor
    /// Note: this could be mPushCounter % 128.
    ///   And modulo 128 should be fast, since it's a power of 2: just take lower 7 bits
    int mDescriptorCounter = 0;

    /// Indicates current page in mPageAddresses
    int mPageIndexCounter = 0;

    /// Amount of data errors detected
    int64_t mErrorCount = 0;

    /// Stream for file readout; only opened if enabled by the --tofile program option
    std::ofstream mReadoutStream;

    /// Stream for idle log; only opened if enabled by the --log-idle program option
    std::ofstream mIdleLogStream;

    /// Stream for log output
    std::ofstream mLogStream;

    /// Stream for error output
    std::ostringstream mErrorStream;

    /// Time of the last display update
    TimePoint mLastDisplayUpdate;

    /// Indicates the display must add a newline to the table
    bool mDisplayUpdateNewline;

    /// Addresses of pages
    std::vector<PageAddress> mPageAddresses;

    struct RandomPausesSoft
    {
        TimePoint next; ///< Next pause at this time
        std::chrono::milliseconds length; ///< Next pause has this length
    } mRandomPausesSoft;

    struct RandomPausesFirm
    {
        bool isPaused = false;
        TimePoint next; ///< Next pause at this time
        std::chrono::milliseconds length; ///< Next pause has this length
    } mRandomPausesFirm;

    /// Set when the DMA loop must be stopped (e.g. SIGINT, max temperature reached)
    bool mDmaLoopBreak = false;

    /// Indicates a SIGINT is being handled
    bool mHandlingSigint = false;

    /// Start time of SIGINT handling
    TimePoint mHandlingSigintStart;

    /// Enables / disables the pushing loop
    bool mPushEnabled = true;

    /// Counter for low priority checks in the loop
    int mLowPriorityCounter = 0;

    /// Low priority counter interval
    static constexpr int LOW_PRIORITY_INTERVAL = 10000;

    /// Queue for page handles
    ReadoutQueue mQueue = ReadoutQueue(QueueBuffer(NUM_PAGES));

    /// Amount of pages pushed in last queue fill. If this is ever greater than 1 during normal operation, it means
    /// we're not keeping the queue filled up all the time, i.e. not keeping up with the card.
    int mLastFillSize = 0;

    int mChannelNumber = 0;

    int64_t mIdleCountCumulative = 0;
    uint32_t mIdleCountLower32 = 0;
    uint32_t mIdleCountUpper32 = 0;
    uint32_t mIdleMaxValue = 0;

    struct IntervalMeasurements
    {
        int pages = 0;
        int polls = 0;
        TimePoint start;

        void reset()
        {
          pages = 0;
          polls = 0;
          start = std::chrono::high_resolution_clock::now();
        }

        double getSecondsSinceStart()
        {
          return std::chrono::duration<double>(std::chrono::high_resolution_clock::now() - start).count();
        }
    } mIntervalMeasurements;
};

} // Anonymous namespace

int main(int argc, char** argv)
{
  return ProgramCruExperimentalDma().execute(argc, argv);
}