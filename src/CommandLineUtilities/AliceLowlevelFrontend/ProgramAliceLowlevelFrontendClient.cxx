/// \file ProgramAliceLowlevelFrontendClient.cxx
/// \brief Utility that starts an example ALICE Lowlevel Frontend (ALF) DIM client
///
/// \author Pascal Boeschoten (pascal.boeschoten@cern.ch)

#include "CommandLineUtilities/Program.h"
#include <iostream>
#include <thread>
#include <dim/dic.hxx>
#include "AliceLowlevelFrontend.h"
#include "AlfException.h"
#include "ServiceNames.h"

using std::cout;
using std::endl;

namespace {
using namespace AliceO2::roc::CommandLineUtilities;

double gTemperature = 0;

class TemperatureInfo: public DimInfo
{
  public:

    TemperatureInfo(const std::string& serviceName)
        : DimInfo(serviceName.c_str(), std::nan(""))
    {
    }

  private:

    void infoHandler() override
    {
      gTemperature = getDouble();
    }
};

class ProgramAliceLowlevelFrontendClient: public Program
{
  public:

    virtual Description getDescription() override
    {
      return {"ALF DIM Client example", "ALICE low-level front-end DIM Client example",
        "roc-alf-client --serial=12345"};
    }

    virtual void addOptions(boost::program_options::options_description& options) override
    {
      Options::addOptionSerialNumber(options);
    }

    virtual void run(const boost::program_options::variables_map& map) override
    {
      // Get DIM DNS node from environment
      if (getenv(std::string("DIM_DNS_NODE").c_str()) == nullptr) {
        BOOST_THROW_EXCEPTION(Alf::AlfException() << Alf::ErrorInfo::Message("Environment variable 'DIM_DNS_NODE' not set"));
      }

      // Get program options
      int serialNumber = Options::getOptionSerialNumber(map);

      // Initialize DIM objects
      Alf::ServiceNames names(serialNumber);
      TemperatureInfo alfTestInt(names.temperature());
      Alf::RegisterReadRpc readRpc(names.registerReadRpc());
      Alf::RegisterWriteRpc writeRpc(names.registerWriteRpc());
      Alf::ScaReadRpc scaReadRpc(names.scaRead());
      Alf::ScaWriteRpc scaWriteRpc(names.scaWrite());
      Alf::ScaGpioReadRpc scaGpioReadRpc(names.scaGpioRead());
      Alf::ScaGpioWriteRpc scaGpioWriteRpc(names.scaGpioWrite());
      Alf::ScaWriteSequence scaWriteSequence(names.scaWriteSequence());
      Alf::PublishRpc publishRpc(names.publishStartCommandRpc());

      publishRpc.publish("ALF/TEST/1", 1.0, {0x1fc});
      publishRpc.publish("ALF/TEST/2", 3.0, {0x100, 0x104, 0x108});

      for (int i = 0; i < 10; ++i) {
        cout << "SCA GPIO write '" << i << "'" << endl;
        cout << "  result: " << scaGpioWriteRpc.write(i) << endl;
        cout << "SCA GPIO read" << endl;
        cout << "  result: " << scaGpioReadRpc.read() << endl;
      }

      {
        cout << "1k writes to 0x1fc..." << endl;
        for (int i = 0; i < 1000; ++i) {
          readRpc.readRegister(0x1fc);
        }
        cout << "Done!" << endl;
      }

      {
        size_t numInts = 4;
        cout << "Writing blob of " << numInts << " pairs of 32-bit ints..." << endl;
        std::vector<std::pair<uint32_t, uint32_t>> buffer(numInts);
        for (size_t i = 0; i < buffer.size(); ++i) {
          buffer[i] = {i * 2, i * 2 + 1};
        }

        std::string result = scaWriteSequence.write(buffer);
        cout << "Done!" << endl;
        cout << "Got result: \n";
        cout << "  " << result << '\n';
      }

      {
        cout << "Writing blob with comments..." << endl;
        std::string result = scaWriteSequence.write("# Hello!\n11,22\n33,44\n# Bye!");
        cout << "Done!" << endl;
        cout << "Got result: \n";
        cout << "  " << result << '\n';
      }

      while (false)//!isSigInt())
      {
        cout << "-------------------------------------\n";
        cout << "Temperature   = " << gTemperature << endl;

        int writes = 10; //std::rand() % 50;
        cout << "Write   0x1f8 = 0x1 times " << writes << endl;
        for (int i = 0; i < writes; ++i) {
          writeRpc.writeRegister(0x1f8, 0x1);
        }

        cout << "Read    0x1fc = " << readRpc.readRegister(0x1fc) << endl;
        cout << "Read    0x1ec = " << readRpc.readRegister(0x1ec) << endl;
        cout << "Cmd     0x1f4 = 0x1" << endl;
        writeRpc.writeRegister(0x1f4, 0x1);
        cout << "Cmd     0x1f4 = 0x2" << endl;
        writeRpc.writeRegister(0x1f4, 0x1);
        cout << "Cmd     0x1f4 = 0x3" << endl;
        writeRpc.writeRegister(0x1f4, 0x1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
      }

      Alf::PublishStopRpc publishStopRpc(names.publishStopCommandRpc());
      publishStopRpc.stop("ALF/TEST/1");
      publishStopRpc.stop("ALF/TEST/2");
    }
};
} // Anonymous namespace

int main(int argc, char** argv)
{
  return ProgramAliceLowlevelFrontendClient().execute(argc, argv);
}
