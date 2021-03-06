/// @file poll2_core.cpp
/// @brief Controls the poll2 command interpreter and data acquisition system
///     The Poll class is used to control the command interpreter and data acqusition systems. Command input and the
///     command line interface of poll2 are handled by the external library CTerminal. Pixie16 data acquisition is
///     handled by interfacing with the PixieInterface library.
/// @author S. V. Paulauskas, Cory R. Thornsberry, Karl Smith, David Miller, Robert Grzywacz
/// @date April 25, 2017

#include <poll2_core.h>
#include <poll2_socket.h>
#include <poll2_stats.h>

#include <CTerminal.h>
#include <Display.h>
#include <EmulatedInterface.hpp>
#include <McaRoot.hpp>
#include <PaassExceptions.hpp>
#include <PixieInterface.h>
#include <PixieSupport.h>
#include <StringManipulationFunctions.hpp>
#include <Utility.h>

#include <algorithm>
#include <iostream>
#include <iomanip>
#include <fstream>
#include <stdexcept>
#include <string>

#include <cstring>
#include <cstdlib>
#include <sstream>
#include <ctime>

#include <cmath>

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

// Values associated with the minimum timing between pixie calls (in us)
// Adjusted to help alleviate the issue with data corruption
#define POLL_TRIES 100

// 2 GB. Maximum allowable .ldf file size in bytes
#define MAX_FILE_SIZE 2147483648ll

std::vector<std::string> chan_params = {"TRIGGER_RISETIME", "TRIGGER_FLATTOP", "TRIGGER_THRESHOLD", "ENERGY_RISETIME",
                                        "ENERGY_FLATTOP", "TAU", "TRACE_LENGTH", "TRACE_DELAY", "VOFFSET", "XDT",
                                        "BASELINE_PERCENT", "EMIN", "BINFACTOR", "CHANNEL_CSRA", "CHANNEL_CSRB", "BLCUT",
                                        "ExternDelayLen", "ExtTrigStretch", "ChanTrigStretch", "FtrigoutDelay",
                                        "FASTTRIGBACKLEN", "CFDDelay", "CFDScale", "CFDThresh", "QDCLen0", "QDCLen1",
                                        "QDCLen2", "QDCLen3", "QDCLen4", "QDCLen5", "QDCLen6", "QDCLen7", "VetoStretch",
                                        "MultiplicityMaskL", "MultiplicityMaskH"};

std::vector<std::string> mod_params = {"MODULE_CSRA", "MODULE_CSRB", "MODULE_FORMAT", "MAX_EVENTS", "SYNCH_WAIT", "IN_SYNCH",
                                       "SLOW_FILTER_RANGE", "FAST_FILTER_RANGE", "ModuleID", "TrigConfig0",
                                       "TrigConfig1", "TrigConfig2","TrigConfig3", "FastTrigBackplaneEna", "CrateID",
                                       "SlotID", "HOST_RT_PRESET"};

const std::vector<std::string> Poll::runControlCommands_ ({"run", "stop", "startacq", "startvme", "stopacq", "stopvme",
                                                           "timedrun", "shm", "spill", "hup", "prefix", "fdir",
                                                           "title", "runnum", "close", "reboot", "stats", "mca"});

const std::vector<std::string> Poll::paramControlCommands_ ({"dump", "pread", "pmread", "pwrite", "pmwrite",
                                                             "adjust_offsets", "find_tau", "toggle", "toggle_bit",
                                                             "csr_test", "bit_test", "get_traces", "save"});

const std::vector<std::string> Poll::pollStatusCommands_ ({"status", "thresh", "debug", "quiet", "quit", "help"});

Poll::Poll() :
        sys_message_head(" POLL: "),
        kill_all(false),
        do_start_acq(false),
        do_stop_acq(false),
        record_data(false),
        do_reboot(false),
        force_spill(false),
        acq_running(false),
        run_ctrl_exit(false),
        had_error(false),
        file_open(false),
        raw_time(0),
        doMcaRun_(false),
        isMcaRunning_(false),
        mcaRunLengthInSeconds_(10),
        mcaBasename_("mca"),
        boot_fast(false),
        insert_wall_clock(true),
        is_quiet(false),
        send_alarm(false),
        show_module_rates(false),
        zero_clocks(false),
        debug_mode(false),
        shm_mode(false),
        init_(false),
        runTime(-1.0),
        output_directory("./"),
        filename_prefix("run"),
        output_title("PIXIE data file"),
        next_run_num(1),
        output_format(0),
        current_file_num(0)
{
    // Check the scheduler (kernel priority)
    Display::LeaderPrint("Checking scheduler");
    int startScheduler = sched_getscheduler(0);

    if(startScheduler == SCHED_BATCH)
        std::cout << Display::InfoStr("SCHED_BATCH") << std::endl;
    else if(startScheduler == SCHED_OTHER)
        std::cout << Display::InfoStr("STANDARD (SCHED_OTHER)") << std::endl;
    else
        std::cout << Display::WarningStr("UNEXPECTED") << std::endl;

    client = new Client();
}

Poll::~Poll() {
    if(init_)
        Close();
    delete pif_;
}

void Poll::PrintModuleInfo() {
    for (int mod=0; mod<pif_->GetConfiguration().GetNumberOfModules(); mod++) {
        unsigned short revision, adcBits, adcMsps;
        unsigned int serialNumber;
        if (pif_->GetModuleInfo((unsigned short)mod, &revision, &serialNumber, &adcBits, &adcMsps)) {
            std::cout << "Module " << std::right << std::setw(2) << mod << ": " <<
                      "Serial Number " << std::right << std::setw(4) << serialNumber << ", " <<
                      "Rev " << std::hex << std::uppercase << revision << std::dec << " " <<
                      "(" << revision << "), " << adcBits << "-bit " << adcMsps << " MS/s " << std::endl;
        }
    }
}

void Poll::SetThreshWords(const double &thresholdPercentage) {
    threshWords = size_t(EXTERNAL_FIFO_LENGTH * thresholdPercentage / 100.);
    std::cout << "Using FIFO threshold of " << thresholdPercentage << "% (" << threshWords << "/"
              << EXTERNAL_FIFO_LENGTH << " words).\n";
}

void Poll::Initialize(const char *configurationFile, const bool &usePixieInterface/* = true*/) {
    if(init_)
        throw InitializationException("Poll::Initialize - Tried to initialize Poll twice! Why'd you do that??");

    try {
        if(usePixieInterface)
            pif_ = new PixieInterface(configurationFile);
        else
            pif_ = new EmulatedInterface(configurationFile);
    } catch (std::invalid_argument &invalidArgument) {
        throw invalidArgument;
    }

    if(debug_mode) {
        std::cout << sys_message_head << "Setting debug mode\n";
        output_file.SetDebugMode();
    }

    if(!pif_->Init())
        throw InitializationException("Poll::Initialize - The interface failed to initialize properly");

    PrintModuleInfo();

    if(boot_fast) {
        if (!pif_->Boot(Interface::BootFlags::DownloadParameters | Interface::BootFlags::SetDAC | Interface::BootFlags::ProgramFPGA, false))
            throw BootException("Poll::Initialize - We couldn't fast boot the modules for some reason!");
    } else {
        if (!pif_->Boot(Interface::BootFlags::BootAll, false))
            throw BootException("Poll::Initialize - We couldn't boot the module for some reason!");
    }

    if(!synch_mods())
        throw SynchronizationException("Poll::Initialize - We couldn't synchronize the modules!");

    n_cards = pif_->GetConfiguration().GetNumberOfModules();

    client->Init("127.0.0.1", 5555);
    partialEvents = new std::vector<Pixie16::word_t>[n_cards];
    statsHandler = new StatsHandler(n_cards);
    statsHandler->SetDumpInterval(statsInterval_);

    commands_.insert(commands_.begin(), pollStatusCommands_.begin(), pollStatusCommands_.end());
    commands_.insert(commands_.begin(), paramControlCommands_.begin(), paramControlCommands_.end());
    commands_.insert(commands_.begin(), runControlCommands_.begin(), runControlCommands_.end());

    init_ = true;
}

bool Poll::Close() {
    //We return if the class has not been initialized.
    if(!init_)
        return false;

    //Send message to Cory's SHM that we are closing.
    client->SendMessage((char *)"$KILL_SOCKET", 13);
    //Close the UDP data / SHM port.
    client->Close();

    // Close any open files.
    if(output_file.IsOpen()) CloseOutputFile();

    //Delete the array of partial event vectors.
    delete[] partialEvents;
    partialEvents = nullptr;

    delete statsHandler;
    statsHandler = nullptr;

    // We are no longer initialized.
    init_ = false;

    return true;
}

bool Poll::CloseOutputFile(const bool &continueRun /*=false*/) {
    Display::LeaderPrint("Closing output file");

    if(!output_file.IsOpen()) {
        std::cout << Display::WarningStr() << std::endl;
        std::cout << "|- No file is open.\n";
        file_open = false;
        return false;
    }

    if (!continueRun) {
        statsHandler->Clear();
        statsHandler->Dump();
    }

    output_file.CloseFile();

    //Broadcast to Cory's SHM that the file is now closed.
    client->SendMessage((char *)"$CLOSE_FILE", 12);

    file_open = false;
    std::cout << Display::OkayStr() << std::endl;

    if (!continueRun)
        output_file.GetNextFileName(next_run_num,filename_prefix,output_directory);

    return true;
}

bool Poll::OpenOutputFile(bool continueRun) {
    Display::LeaderPrint("Opening output file");

    if(output_file.IsOpen()) {
        std::cout << Display::ErrorStr() << std::endl;
        std::cout << "|- A file is already open!\n";
        CloseOutputFile();

        had_error = true;
        record_data = false;

        return false;
    }

    if(!output_file.OpenNewFile(output_title, next_run_num, filename_prefix, output_directory, continueRun)) {
        std::cout << Display::ErrorStr() << std::endl;
        std::cout << "|- Failed to open output file! Check that the path is correct.\n";
        std::cout << "|- Filename: '" << output_file.GetCurrentFilename() << "'.\n";

        had_error = true;
        record_data = false;

        return false;
    }
    std::cout <<Display::OkayStr() <<std::endl;
    std::cout << "|- Filename: '" << output_file.GetCurrentFilename() << "'.\n";

    statsHandler->Clear();
    statsHandler->Dump();

    client->SendMessage((char *)"$OPEN_FILE", 12);
    file_open = true;
    return true;
}

bool Poll::synch_mods() {
    static bool firstTime = true;
    static char synchString[] = "IN_SYNCH";
    static char waitString[] = "SYNCH_WAIT";

    bool hadError = false;
    Display::LeaderPrint("Synchronizing");

    if(firstTime) {
        // only need to set this in the first module once
        if(!pif_->WriteSglModPar(waitString, 1, 0)) { hadError = true; }
        firstTime = false;
    }

    for(unsigned int mod = 0; mod < pif_->GetConfiguration().GetNumberOfModules(); mod++)
        if (!pif_->WriteSglModPar(synchString, 0, mod))
            hadError = true;

    if (!hadError)
        std::cout << Display::OkayStr() << std::endl;
    else
        std::cout << Display::ErrorStr() << std::endl;

    return !hadError;
}

int Poll::write_data(Pixie16::word_t *data, unsigned int nWords) {
    if(!output_file.IsOpen()) {
        std::cout << Display::ErrorStr() << " Recording data, but no file is open!\n";
        do_stop_acq = true;
        had_error = true;
        return 0;
    }

    // Handle the writing of buffers to the file
    //65552 = 8194 * 4 * 2 , 2 EOF buffers are need 8194 words at 4 bytes per word
    std::streampos current_filesize = output_file.GetFilesize();
    if(current_filesize + (std::streampos)(4*nWords + 65552) > MAX_FILE_SIZE) {
        // Adding nWords plus 2 EOF buffers to the file will push it over MAX_FILE_SIZE.
        // Open a new output file instead
        std::cout << sys_message_head << "Maximum ifile size reached. New output file will be created.\n";
        std::cout << sys_message_head << "Current filesize is " << current_filesize + (std::streampos)65552 << " bytes.\n";
        CloseOutputFile(true);
        OpenOutputFile(true);
    }

    if (!is_quiet)
        std::cout << "Writing " << nWords << " words.\n";

    return output_file.Write((char*)data, nWords);
}

void Poll::broadcast_data(Pixie16::word_t *data, unsigned int nWords) {
    // Maximum size of the shared memory buffer
    static const unsigned int maxShmSizeL = 4050; // in pixie words
    static const unsigned int maxShmSize  = maxShmSizeL * sizeof(Pixie16::word_t); // in bytes

    if(shm_mode) { // Broadcast the spill onto the network using the new shm style
        int shm_data[maxShmSizeL+2]; // packets of data
        unsigned int num_net_chunks = nWords / maxShmSizeL;
        unsigned int num_net_remain = nWords % maxShmSizeL;

        if(num_net_remain != 0)
            num_net_chunks++;

        unsigned int net_chunk = 1;
        unsigned int words_bcast = 0;
        if(debug_mode)
            std::cout << " debug: Splitting " << nWords << " words into network spill of " << num_net_chunks
                      << " chunks (fragment = " << num_net_remain << " words)\n";

        while(words_bcast < nWords) {
            if(nWords - words_bcast > maxShmSizeL) { // Broadcast the spill chunks
                memcpy(&shm_data[0], &net_chunk, 4);
                memcpy(&shm_data[1], &num_net_chunks, 4);
                memcpy(&shm_data[2], &data[words_bcast], maxShmSize);
                client->SendMessage((char *)shm_data, maxShmSize+8);
                words_bcast += maxShmSizeL;
            } else{ // Broadcast the spill remainder
                memcpy(&shm_data[0], &net_chunk, 4);
                memcpy(&shm_data[1], &num_net_chunks, 4);
                memcpy(&shm_data[2], &data[words_bcast], (nWords-words_bcast)*4);
                client->SendMessage((char *)shm_data, (nWords - words_bcast + 2)*4);
                words_bcast += nWords-words_bcast;
            }
            usleep(1);
            net_chunk++;
        }
    } else{ // Broadcast a spill notification to the network
        output_file.SendPacket(client);
    }
}

void Poll::help() {
    std::cout << "  Help:\n";
    std::cout << "   run                 - Start data acquisition and start recording data to disk\n";
    std::cout << "   stop                - Stop data acquisition and stop recording data to disk\n";
    std::cout << "   startacq (startvme) - Start data acquisition\n";
    std::cout << "   stopacq (stopvme)   - Stop data acquisition\n";
    std::cout << "   timedrun <seconds>  - Run for the specified number of seconds\n";
    std::cout << "   acq (shm)           - Run in \"shared-memory\" mode\n";
    std::cout << "   spill (hup)         - Force dump of current spill\n";
    std::cout << "   prefix [name]       - Set the output filename prefix (default='run_#.ldf')\n";
    std::cout << "   fdir [path]         - Set the output file directory (default='./')\n";
    std::cout << "   title [runTitle]    - Set the title of the current run (default='PIXIE Data File)\n";
    std::cout << "   runnum [number]     - Set the number of the current run (default=0)\n";
    std::cout << "   reboot              - Reboot PIXIE crate\n";
    std::cout << "   stats [time]        - Set the time delay between statistics dumps (default=-1)\n";
    std::cout << "   mca [time] [filename]                 - Use MCA to record data. time = 0 starts an infinite run\n";
    std::cout << "   dump [filename]                       - Dump pixie settings to file (default='Fallback.set')\n";
    std::cout << "   pread <mod> <chan> <param>            - Read parameters from individual PIXIE channels\n";
    std::cout << "   pmread <mod> <param>                  - Read parameters from PIXIE modules\n";
    std::cout << "   pwrite <mod> <chan> <param> <val>     - Write parameters to individual PIXIE channels\n";
    std::cout << "   pmwrite <mod> <param> <val>           - Write parameters to PIXIE modules\n";
    std::cout << "   adjust_offsets <module>               - Adjusts the baselines of a pixie module\n";
    std::cout << "   find_tau <module> <channel>           - Finds the decay constant for an active pixie channel\n";
    std::cout << "   toggle <module> <channel> <bit>       - Toggle any of the 19 CHANNEL_CSRA bits for a pixie channel\n";
    std::cout << "   toggle_bit <mod> <chan> <param> <bit> - Toggle any bit of any parameter of 32 bits or less\n";
    std::cout << "   csr_test <number>                     - Output the CSRA parameters for a given integer\n";
    std::cout << "   bit_test <num_bits> <number>          - Display active bits in a given integer up to 32 bits long\n";
    std::cout << "   save [setFilename]                    - Writes the DSP Parameters to [setFileName] (default='active .set from pixie_cfg')\n";
    std::cout << "   get_traces <mod> <chan> [threshold]   - Get traces for all channels in a specified module\n";
    std::cout << "   status              - Display system status information\n";
    std::cout << "   thresh [threshold]  - Modify or display the current polling threshold.\n";
    std::cout << "   debug               - Toggle debug mode flag (default=false)\n";
    std::cout << "   quiet               - Toggle quiet mode flag (default=false)\n";
    std::cout << "   quit                - Close the program\n";
    std::cout << "   help (h)            - Display this dialogue\n";
}

void Poll::save_help() {
    std::cout << "  Saves the DSP parameters to disk. Optionally, a file can be provided, otherwise the file set file"
                 " from pixie.cfg is used.\n";
}

/* Print help dialogue for reading/writing pixie channel parameters. */
void Poll::pchan_help() {
    std::cout << "  Valid Pixie16 channel parameters:\n";
    for(const auto &val : chan_params)
        std::cout << "   " << val << "\n";
}

/* Print help dialogue for reading/writing pixie module parameters. */
void Poll::pmod_help() {
    std::cout << "  Valid Pixie16 module parameters:\n";
    for(const auto &val : mod_params)
        std::cout << "   " << val << "\n";
}

bool Poll::start_run(const bool &record_/*=true*/, const double &time_/*=-1.0*/) {
    if(doMcaRun_) {
        std::cout << sys_message_head << "Warning! Cannot run acquisition while MCA program is running\n";
        return false;
    } else if(acq_running) {
        std::cout << sys_message_head << "Acquisition is already running\n";
        return false;
    }

    runTime = time_;

    if(runTime > 0.0)
        std::cout << sys_message_head << "Running for approximately " << runTime << " seconds.\n";

    record_data = record_;

    do_start_acq = true;

    return true;
}

bool Poll::stop_run() {
    if(!acq_running && !doMcaRun_) {
        std::cout << sys_message_head << "Acquisition is not running\n";
        return false;
    }

    do_stop_acq = true;

    if (record_data) {
        std::stringstream output;
        output << "Run " << output_file.GetRunNumber() << " time";
        Display::LeaderPrint(output.str());
        std::cout << statsHandler->GetTotalTime() << "s\n";
    }

    record_data = false;

    return true;
}

void Poll::show_status() {
    std::cout << "  Poll Run Status:\n";
    std::cout << "   Acq starting    - " << StringManipulation::BoolToString(do_start_acq) << std::endl;
    std::cout << "   Acq stopping    - " << StringManipulation::BoolToString(do_stop_acq) << std::endl;
    std::cout << "   Acq running     - " << StringManipulation::BoolToString(acq_running) << std::endl;
    std::cout << "   Shared memory   - " << StringManipulation::BoolToString(shm_mode) << std::endl;
    std::cout << "   Write to disk   - " << StringManipulation::BoolToString(record_data) << std::endl;
    std::cout << "   File open       - " << StringManipulation::BoolToString(output_file.IsOpen()) << std::endl;
    std::cout << "   Rebooting       - " << StringManipulation::BoolToString(do_reboot) << std::endl;
    std::cout << "   Force Spill     - " << StringManipulation::BoolToString(force_spill) << std::endl;
    std::cout << "   Do MCA run      - " << StringManipulation::BoolToString(doMcaRun_) << std::endl;
    std::cout << "   Run ctrl Exited - " << StringManipulation::BoolToString(run_ctrl_exit) << std::endl;

    std::cout << "\n  Poll Options:\n";
    std::cout << "   Boot fast   - " << StringManipulation::BoolToString(boot_fast) << std::endl;
    std::cout << "   Wall clock  - " << StringManipulation::BoolToString(insert_wall_clock) << std::endl;
    std::cout << "   Is quiet    - " << StringManipulation::BoolToString(is_quiet) << std::endl;
    std::cout << "   Send alarm  - " << StringManipulation::BoolToString(send_alarm) << std::endl;
    std::cout << "   Show rates  - " << StringManipulation::BoolToString(show_module_rates) << std::endl;
    std::cout << "   Zero clocks - " << StringManipulation::BoolToString(zero_clocks) << std::endl;
    std::cout << "   Debug mode  - " << StringManipulation::BoolToString(debug_mode) << std::endl;
    std::cout << "   Initialized - " << StringManipulation::BoolToString(init_) << std::endl;
}

void Poll::show_thresh() {
    float threshPercent = (float) threshWords / EXTERNAL_FIFO_LENGTH * 100;
    std::cout << sys_message_head << "Polling Threshold = " << threshPercent << "% (" << threshWords << "/" << EXTERNAL_FIFO_LENGTH << ")\n";
}

void Poll::get_traces(int mod_, int chan_, int thresh_/*=0*/) {
    size_t trace_size = PixieInterface::GetTraceLength();
    size_t module_size = pif_->GetConfiguration().GetNumberOfChannels() * trace_size;
    std::cout << sys_message_head << "Searching for traces from mod = " << mod_ << ", chan = " << chan_ << " above threshold = " << thresh_ << ".\n";
    std::cout << sys_message_head << "Allocating " << (trace_size+module_size)*sizeof(unsigned short) << " bytes of memory for pixie traces.\n";
    std::cout << sys_message_head << "Searching for traces. Please wait...\n";
    poll_term_->flush();

    auto *trace_data = new unsigned short[trace_size];
    auto *module_data = new unsigned short[module_size];
    memset(trace_data, 0, sizeof(unsigned short)*trace_size);
    memset(module_data, 0, sizeof(unsigned short)*module_size);

    GetTraces gtraces(module_data, module_size, trace_data, trace_size, thresh_);
    forChannel(pif_, mod_, chan_, gtraces, (int)0);

    if(!gtraces.GetStatus())
        std::cout << sys_message_head << "Failed to find trace above threshold in " << gtraces.GetAttempts() << " attempts!\n";
    else
        std::cout << sys_message_head << "Found trace above threshold in " << gtraces.GetAttempts() << " attempts.\n";

    std::cout << "  Baselines:\n";
    for(unsigned int channel = 0; channel < pif_->GetConfiguration().GetNumberOfChannels(); channel++) {
        if(channel == (unsigned)chan_)
            std::cout << "\033[0;33m";

        if(channel < 10)
            std::cout << "   0" << channel << ": ";
        else
            std::cout << "   " << channel << ": ";

        std::cout << "\t" << gtraces.GetBaseline(channel);
        std::cout << "\t" << gtraces.GetMaximum(channel) << std::endl;
        if(channel == (unsigned)chan_)
            std::cout << "\033[0m";
    }

    std::ofstream get_traces_out("/tmp/traces.dat");
    if(!get_traces_out.good())
        std::cout << sys_message_head << "Could not open /tmp/traces.dat!\n";
    else { // Write the output file.
        // Add a header.
        get_traces_out << "time";
        for(size_t channel = 0; channel < pif_->GetConfiguration().GetNumberOfChannels(); channel++) {
            if(channel < 10)
                get_traces_out << "\tC0" << channel;
            else
                get_traces_out << "\tC" << channel;
        }
        get_traces_out << std::endl;

        // Write channel traces.
        for(size_t index = 0; index < trace_size; index++) {
            get_traces_out << index;

            for(size_t channel = 0; channel < pif_->GetConfiguration().GetNumberOfChannels(); channel++)
                get_traces_out << "\t" << module_data[(channel * trace_size) + index];

            get_traces_out << std::endl;
        }
        std::cout << sys_message_head << "Traces written to '/tmp/traces.dat'." << std::endl;
    }
    get_traces_out.close();

    delete[] trace_data;
    delete[] module_data;
}

bool Poll::SplitParameterArgs(const std::string &arg, int &start, int &stop) {
    //If a character is found that is nonnumerical or is not the delimeter we stop.
    if (arg.find_first_not_of("-0123456789:") != std::string::npos)
        return false;

    size_t delimeterPos = arg.find(':');
    try {
        start = std::stoi(arg.substr(0, delimeterPos));
        //If the delimiter was found we can separate the stop otherwise set start = stop.
        if (delimeterPos != std::string::npos) {
            stop = std::stoi(arg.substr(delimeterPos + 1));
            if (start < 0 || stop < 0 || start > stop)
                return false;
        } else
            stop = start;
    } catch (const std::invalid_argument &ia) {
        return false;
    }
    return true;
}

void Poll::CommandControl() {
    auto cmd = std::string("");
    std::string arg;

    while(true) {
        if(kill_all) { // Check if poll has been killed externally (pacman)
            while(!run_ctrl_exit) { sleep(1); }
            break;
        }

        cmd = poll_term_->GetCommand(arg);
        if(cmd == "_SIGSEGV_") {
            std::cout << Display::ErrorStr("SEGMENTATION FAULT") << std::endl;
            Close();
            exit(EXIT_FAILURE);
        } else if(cmd == "CTRL_D") {
            std::cout << sys_message_head << "Received EOF (ctrl-d) signal. Exiting...\n";
            cmd = "quit";
        } else if(cmd == "CTRL_C") {
            std::cout << sys_message_head << "Received SIGINT (ctrl-c) signal.";
            if (doMcaRun_) {
                std::cout << " Stopping MCA...\n";
                cmd = "stop";
            } else {
                std::cout << " Ignoring signal.\n";
                continue;
            }
        } else if(cmd == "CTRL_Z") {
            std::cout << sys_message_head << "Warning! Received SIGTSTP (ctrl-z) signal.\n";
            continue;
        }

        if (cmd.find('\t') != std::string::npos) { // Completing a command.
            poll_term_->TabComplete(cmd, commands_);
            continue;
        } else if (arg.find('\t') != std::string::npos) { // Completing the argument.
            if(cmd == "pread" || cmd == "pwrite")
                poll_term_->TabComplete(arg, chan_params);
            else if(cmd == "pmread" || cmd == "pmwrite")
                poll_term_->TabComplete(arg, mod_params);
            else if(cmd == "toggle")
                poll_term_->TabComplete(arg, BitFlipper::toggle_names);
            continue;
        }
        poll_term_->flush();

        if(cmd.empty())
            continue;

        std::vector<std::string> arguments;
        unsigned int p_args = split_str(arg, arguments);

        //We clear the error flag when a command is entered.
        had_error = false;
        // check for defined commands
        if(cmd == "quit" || cmd == "exit") {
            if(doMcaRun_)
                std::cout << sys_message_head << "Warning! Cannot quit while MCA program is running\n";
            else if(acq_running)
                std::cout << sys_message_head << "Warning! Cannot quit while acquisition running\n";
            else {
                kill_all = true;
                while(!run_ctrl_exit) { sleep(1); }
                break;
            }
        } else if(cmd == "kill") {
            if(acq_running || doMcaRun_) {
                std::cout << sys_message_head << "Sending KILL signal\n";
                do_stop_acq = true;
            }
            kill_all = true;
            while(!run_ctrl_exit)
                sleep(1);
            break;
        } else if(cmd == "help" || cmd == "h") {
            help();
        } else if(cmd == "status") {
            show_status();
        } else if(cmd == "thresh") {
            if(p_args==1) {
                if(!StringManipulation::IsNumeric(arguments.at(0))) {
                    std::cout << sys_message_head << " Invalid FIFO threshold specification" << std::endl;
                    continue;
                }
                SetThreshWords(std::stod(arguments.at(0)));
            }
            show_thresh();
        } else if(cmd == "dump") { // Dump pixie parameters to file
            std::ofstream ofile;

            if(p_args >= 1) {
                ofile.open(arg.c_str());
                if(!ofile.good()) {
                    std::cout << sys_message_head << "Failed to open output file '" << arg << "'\n";
                    std::cout << sys_message_head << "Check that the path is correct\n";
                    continue;
                }
            } else{
                ofile.open("./Fallback.set");
                if(!ofile.good()) {
                    std::cout << sys_message_head << "Failed to open output file './Fallback.set'\n";
                    continue;
                }
            }

            ParameterChannelDumper chanReader(&ofile);
            ParameterModuleDumper modReader(&ofile);

            // Channel dependent settings
            for(auto &val : chan_params)
                forChannel<std::string>(pif_, -1, -1, chanReader, val);

            // Channel independent settings
            for(auto &val : mod_params)
                forModule(pif_, -1, modReader, val);

            if(p_args >= 1)
                std::cout << sys_message_head << "Successfully wrote output parameter file '" << arg << "'\n";
            else
                std::cout << sys_message_head << "Successfully wrote output parameter file './Fallback.set'\n";
            ofile.close();
        } else if(cmd == "pwrite" || cmd == "pmwrite") { // Write pixie parameters
            if(acq_running || doMcaRun_) {
                std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n";
                continue;
            }

            if(cmd == "pwrite") { // Syntax "pwrite <module> <channel> <parameter name> <value>"
                if(p_args > 0 && arguments.at(0) == "help")
                    pchan_help();
                else if(p_args >= 4) {
                    int modStart, modStop;
                    if (!SplitParameterArgs(arguments.at(0), modStart, modStop)) {
                        std::cout << "ERROR: Invalid module argument: '" << arguments.at(0) << "'\n";
                        continue;
                    }
                    int chStart, chStop;
                    if (!SplitParameterArgs(arguments.at(1), chStart, chStop)) {
                        std::cout << "ERROR: Invalid channel argument: '" << arguments.at(1) << "'\n";
                        continue;
                    }

                    //Check that there are no characters in the string unless it is hex.
                    std::string &valueStr = arguments.at(3);
                    if (valueStr.find_last_not_of("+-eE0123456789.") != std::string::npos &&
                        !((valueStr.find("0x") == 0 || valueStr.find("0X") == 0) &&
                          valueStr.find_first_not_of("0123456789abcdefABCDEF", 2) == std::string::npos) ) {
                        std::cout << "ERROR: Invalid parameter value: '" << valueStr << "'\n";
                        continue;
                    }

                    double value;
                    try {
                        value = std::stod(valueStr);
                    } catch (const std::invalid_argument &ia) {
                        std::cout << "ERROR: Invalid parameter value: '" << valueStr << "'\n";
                        continue;
                    }

                    ParameterChannelWriter writer;
                    bool error = false;
                    for (int mod = modStart; mod <= modStop; mod++)
                        for (int ch = chStart; ch <= chStop; ch++)
                            if( ! forChannel(pif_, mod, ch, writer, make_pair(arguments.at(2), value)))
                                error = true;
                    if (!error)
                        pif_->SaveDSPParameters();
                } else{
                    std::cout << sys_message_head << "Invalid number of parameters to pwrite\n";
                    std::cout << sys_message_head << " -SYNTAX- pwrite <module> <channel> <parameter> <value>\n";
                }
            } else if(cmd == "pmwrite") { // Syntax "pmwrite <module> <parameter name> <value>"
                if(p_args > 0 && arguments.at(0) == "help")
                    pmod_help();
                else if(p_args >= 3) {
                    int modStart, modStop;
                    if (!SplitParameterArgs(arguments.at(0), modStart, modStop)) {
                        std::cout << "ERROR: Invalid module argument: '" << arguments.at(0) << "'\n";
                        continue;
                    }

                    //Check that there are no characters in the string unless it is hex.
                    std::string &valueStr = arguments.at(2);
                    if (valueStr.find_last_not_of("0123456789") != std::string::npos &&
                        !((valueStr.find("0x") == 0 || valueStr.find("0X") == 0) &&
                          valueStr.find_first_not_of("0123456789abcdefABCDEF", 2) == std::string::npos) ) {
                        std::cout << "ERROR: Invalid parameter value: '" << valueStr << "'\n";
                        continue;
                    }

                    unsigned int value;
                    try {
                        value = (unsigned int) std::stod(valueStr);
                    } catch (const std::invalid_argument &ia) {
                        std::cout << "ERROR: Invalid parameter value: '" << valueStr << "'\n";
                        continue;
                    }

                    ParameterModuleWriter writer;
                    bool error = false;
                    for (int mod = modStart; mod <= modStop; mod++)
                        if(!forModule(pif_, mod, writer, make_pair(arguments.at(1), value)))
                            error = true;

                    if (!error)
                        pif_->SaveDSPParameters();
                } else{
                    std::cout << sys_message_head << "Invalid number of parameters to pmwrite\n";
                    std::cout << sys_message_head << " -SYNTAX- pmwrite <module> <parameter> <value>\n";
                }
            }
        } else if (cmd == "save") {
            if(acq_running || doMcaRun_) {
                std::cout << sys_message_head << "Warning! Cannot view pixie parameters while acquisition is running\n\n";
                continue;
            }

            if(p_args > 0 && arguments.at(0) == "help") {
                save_help();
                continue;
            }

            if(p_args == 0)
                pif_->SaveDSPParameters();
            else if (p_args == 1)
                pif_->SaveDSPParameters(arguments.at(0).c_str());
            else {
                std::cout << sys_message_head << "Invalid number of parameters to save\n";
                std::cout << sys_message_head << " -SYNTAX- save [setFilename]\n";
                continue;
            }
        } else if(cmd == "pread" || cmd == "pmread") { // Read pixie parameters
            if(acq_running || doMcaRun_) {
                std::cout << sys_message_head << "Warning! Cannot view pixie parameters while acquisition is running\n\n";
                continue;
            }

            if(cmd == "pread") { // Syntax "pread <module> <channel> <parameter name>"
                if(p_args > 0 && arguments.at(0) == "help")
                    pchan_help();
                else if(p_args >= 3) {
                    int modStart, modStop;
                    if (!SplitParameterArgs(arguments.at(0), modStart, modStop)) {
                        std::cout << "ERROR: Invalid module argument: '" << arguments.at(0) << "'\n";
                        continue;
                    }
                    int chStart, chStop;
                    if (!SplitParameterArgs(arguments.at(1), chStart, chStop)) {
                        std::cout << "ERROR: Invalid channel argument: '" << arguments.at(1) << "'\n";
                        continue;
                    }

                    ParameterChannelReader reader;
                    for (int mod = modStart; mod <= modStop; mod++)
                        for (int ch = chStart; ch <= chStop; ch++)
                            forChannel(pif_, mod, ch, reader, arguments.at(2));
                } else{
                    std::cout << sys_message_head << "Invalid number of parameters to pread\n";
                    std::cout << sys_message_head << " -SYNTAX- pread <module> <channel> <parameter>\n";
                }
            } else if(cmd == "pmread") { // Syntax "pmread <module> <parameter name>"
                if(p_args > 0 && arguments.at(0) == "help")
                    pmod_help();
                else if(p_args >= 2) {
                    int modStart, modStop;
                    if (!SplitParameterArgs(arguments.at(0), modStart, modStop)) {
                        std::cout << "ERROR: Invalid module argument: '" << arguments.at(0) << "'\n";
                        continue;
                    }

                    ParameterModuleReader reader;
                    for (int mod = modStart; mod <= modStop; mod++) {
                        forModule(pif_, mod, reader, arguments.at(1));
                    }
                } else{
                    std::cout << sys_message_head << "Invalid number of parameters to pmread\n";
                    std::cout << sys_message_head << " -SYNTAX- pread <module> <parameter>\n";
                }
            }
        } else if(cmd == "adjust_offsets") { // Run adjust_offsets
            if(acq_running || doMcaRun_) {
                std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n";
                continue;
            }

            if(p_args >= 1) {
                int modStart, modStop;
                if (!SplitParameterArgs(arguments.at(0), modStart, modStop)) {
                    std::cout << "ERROR: Invalid module argument: '" << arguments.at(0) << "'\n";
                    continue;
                }

                OffsetAdjuster adjuster;
                bool error = false;
                for (int mod = modStart; mod <= modStop; mod++)
                    if(!forModule(pif_, mod, adjuster, 0))
                        error = true;
                if (!error)
                    pif_->SaveDSPParameters();
            } else{
                std::cout << sys_message_head << "Invalid number of parameters to adjust_offsets\n";
                std::cout << sys_message_head << " -SYNTAX- adjust_offsets <module>\n";
            }
        } else if(cmd == "find_tau") { // Run find_tau
            if(acq_running || doMcaRun_) {
                std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n";
                continue;
            }

            if(p_args >= 2) {
                if(!StringManipulation::IsNumeric(arguments.at(0))) {
                    std::cout << sys_message_head << " Invalid module specification" << std::endl;
                    continue;
                } else if(!StringManipulation::IsNumeric(arguments.at(1))) {
                    std::cout << sys_message_head << " Invalid channel specification" << std::endl;
                    continue;
                }

                int mod = std::stoi(arguments.at(0));
                int ch = std::stoi(arguments.at(1));

                TauFinder finder;
                forChannel(pif_, mod, ch, finder, 0);
            }
            else{
                std::cout << sys_message_head << "Invalid number of parameters to find_tau\n";
                std::cout << sys_message_head << " -SYNTAX- find_tau <module> <channel>\n";
            }
        } else if(cmd == "toggle") { // Toggle a CHANNEL_CSRA bit
            if(acq_running || doMcaRun_) {
                std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n";
                continue;
            }

            BitFlipper flipper;

            if(p_args >= 3) {
                int modStart, modStop;
                if (!SplitParameterArgs(arguments.at(0), modStart, modStop)) {
                    std::cout << "ERROR: Invalid module argument: '" << arguments.at(0) << "'\n";
                    continue;
                }
                int chStart, chStop;
                if (!SplitParameterArgs(arguments.at(1), chStart, chStop)) {
                    std::cout << "ERROR: Invalid channel argument: '" << arguments.at(1) << "'\n";
                    continue;
                }
                flipper.SetCSRAbit(arguments.at(2));

                std::string dum_str = "CHANNEL_CSRA";
                bool error = false;
                for (int mod = modStart; mod <= modStop; mod++)
                    for (int ch = chStart; ch <= chStop; ch++)
                        if(!forChannel(pif_, mod, ch, flipper, dum_str))
                            error = true;
                if (!error)
                    pif_->SaveDSPParameters();
            } else{
                std::cout << sys_message_head << "Invalid number of parameters to toggle\n";
                std::cout << sys_message_head << " -SYNTAX- toggle <module> <channel> <CSRA bit>\n";
                flipper.Help();
            }
        } else if(cmd == "toggle_bit") { // Toggle any bit of any parameter under 32 bits long
            if(acq_running || doMcaRun_) {
                std::cout << sys_message_head << "Warning! Cannot edit pixie parameters while acquisition is running\n\n";
                continue;
            }

            BitFlipper flipper;

            if(p_args >= 4) {
                if(!StringManipulation::IsNumeric(arguments.at(0))) {
                    std::cout << sys_message_head << "Invalid module specification" << std::endl;
                    continue;
                } else if(!StringManipulation::IsNumeric(arguments.at(1))) {
                    std::cout << sys_message_head << " Invalid channel specification" << std::endl;
                    continue;
                } else if(!StringManipulation::IsNumeric(arguments.at(3))) {
                    std::cout << sys_message_head << " Invalid bit number specification" << std::endl;
                    continue;
                }

                flipper.SetBit(arguments.at(3));

                if(forChannel(pif_, std::stoi(arguments.at(0)), std::stoi(arguments.at(1)), flipper, arguments.at(2)))
                    pif_->SaveDSPParameters();
            } else{
                std::cout << sys_message_head << "Invalid number of parameters to toggle_any\n";
                std::cout << sys_message_head << " -SYNTAX- toggle_any <module> <channel> <parameter> <bit>\n";
            }
        } else if(cmd == "csr_test") { // Run CSRAtest method
            BitFlipper flipper;
            if(p_args >= 1) {
                //Check that there are no characters in the string unless it is hex.
                std::string &valueStr = arguments.at(0);
                if (valueStr.find_last_not_of("0123456789") != std::string::npos &&
                    !((valueStr.find("0x") == 0 || valueStr.find("0X") == 0) &&
                      valueStr.find_first_not_of("0123456789abcdefABCDEF", 2) == std::string::npos) ) {
                    std::cout << "ERROR: Invalid parameter value: '" << valueStr << "'\n";
                    continue;
                }
                unsigned int value;
                //Use stod to add hex capability. The decimal and negative values are
                // caught above and rejected.
                try {
                    value = (unsigned int) std::stod(valueStr);
                } catch (const std::invalid_argument &ia) {
                    std::cout << "ERROR: Invalid parameter value: '" << valueStr << "'\n";
                    continue;
                }

                flipper.CSRAtest(value);
            } else{
                std::cout << sys_message_head << "Invalid number of parameters to csr_test\n";
                std::cout << sys_message_head << " -SYNTAX- csr_test <number>\n";
            }
        } else if(cmd == "bit_test") { // Run Test method
            BitFlipper flipper;
            if(p_args >= 2) {
                if(!StringManipulation::IsNumeric(arguments.at(1))) {
                    std::cout << sys_message_head << "Invalid number of bits specified" << std::endl;
                    continue;
                } else if(!StringManipulation::IsNumeric(arguments.at(2))) {
                    std::cout << sys_message_head << " Invalid parameter value specification" << std::endl;
                    continue;
                }
                std::vector<std::string> empty_vector;
                flipper.Test((unsigned int)std::stoi(arguments.at(0)),
                        (unsigned int)std::stoi(arguments.at(1), nullptr, 0), empty_vector);
            } else{
                std::cout << sys_message_head << "Invalid number of parameters to bit_test\n";
                std::cout << sys_message_head << " -SYNTAX- bit_test <num_bits> <number>\n";
            }
        } else if(cmd == "get_traces") { // Run GetTraces method
            if(acq_running || doMcaRun_) {
                std::cout << sys_message_head << "Warning! Cannot view live traces while acquisition is running\n\n";
                continue;
            }

            if(p_args >= 2) {
                if(!StringManipulation::IsNumeric(arguments.at(0))) {
                    std::cout << sys_message_head << "Invalid module specification" << std::endl;
                    continue;
                } else if(!StringManipulation::IsNumeric(arguments.at(1))) {
                    std::cout << sys_message_head << " Invalid channel specification" << std::endl;
                    continue;
                }

                int mod = std::stoi(arguments.at(0));
                int chan = std::stoi(arguments.at(1));

                if(mod < 0 || chan < 0) {
                    std::cout << sys_message_head << "Error! Must select one module and one channel to trigger on!\n";
                    continue;
                } else if(mod > (int)n_cards) {
                    std::cout << sys_message_head << "Error! Invalid module specification (" << mod << ")!\n";
                    continue;
                } else if(chan > NUMBER_OF_CHANNELS) {
                    std::cout << sys_message_head << "Error! Invalid channel specification (" << chan << ")!\n";
                    continue;
                }

                int trace_threshold = 0;
                if(p_args >= 3) {
                    if(!StringManipulation::IsNumeric(arguments.at(2))) {
                        std::cout << sys_message_head << "Invalid threshold specified" << std::endl;
                        continue;
                    } else{
                        trace_threshold = std::stoi(arguments.at(2));
                        if(trace_threshold < 0) {
                            std::cout << sys_message_head << "Cannot set negative threshold!\n";
                            trace_threshold = 0;
                        }
                    }
                }

                get_traces(mod, chan, trace_threshold);
            } else{
                std::cout << sys_message_head << "Invalid number of parameters to get_traces\n";
                std::cout << sys_message_head << " -SYNTAX- get_traces <mod> <chan> [threshold]\n";
            }
        } else if(cmd == "quiet") { // Toggle quiet mode
            if(is_quiet) {
                std::cout << sys_message_head << "Toggling quiet mode OFF\n";
                is_quiet = false;
            } else{
                std::cout << sys_message_head << "Toggling quiet mode ON\n";
                is_quiet = true;
            }
        } else if(cmd == "debug") { // Toggle debug mode
            if(debug_mode) {
                std::cout << sys_message_head << "Toggling debug mode OFF\n";
                output_file.SetDebugMode(false);
                debug_mode = false;
            } else{
                std::cout << sys_message_head << "Toggling debug mode ON\n";
                output_file.SetDebugMode();
                debug_mode = true;
            }
        } else if(cmd == "mca" || cmd == "MCA") {
            if(doMcaRun_) {
                std::cout << sys_message_head << "MCA program is already running\n\n";
                continue;
            }

            if(acq_running) {
                std::cout << sys_message_head << "Warning! Cannot run MCA program while acquisition is running\n\n";
                continue;
            }

            switch(p_args) {
                case 0:
                    mcaRunLengthInSeconds_ = 10;
                    mcaBasename_ = "mca";
                    break;
                case 1:
                    if(StringManipulation::IsNumeric(arguments.at(0))) {
                        mcaRunLengthInSeconds_ = stod(arguments.at(0));
                        mcaBasename_ = "mca";
                        std::cout << sys_message_head << "Setting up a " << mcaRunLengthInSeconds_ << " MCA run into mca.root\n";
                    } else {
                        mcaRunLengthInSeconds_ = 10;
                        mcaBasename_ = arguments.at(0);
                        std::cout << sys_message_head << "Setting up an infinite MCA run into " << mcaBasename_ << "\n";
                    }
                    break;
                case 2:
                    if(StringManipulation::IsNumeric(arguments.at(0))) {
                        mcaRunLengthInSeconds_ = stod(arguments.at(0));
                        mcaBasename_ = arguments.at(1);
                    } else if (StringManipulation::IsNumeric(arguments.at(1))) {
                        mcaRunLengthInSeconds_ = stod(arguments.at(1));
                        mcaBasename_ = arguments.at(0);
                    } else {
                        std::cout << sys_message_head << "mca only accepts a numeric time!!\n";
                        continue;
                    }

                    std::cout << sys_message_head << "Setting up a " << mcaRunLengthInSeconds_ << " MCA run into " << mcaBasename_ << "\n";
                    break;
                default:
                    std::cout << sys_message_head << "Too many arguments provided to MCA! Ignoring additional args.\n";
            }
            doMcaRun_ = true;
        } else if(cmd == "run") { // Tell POLL to start acq and start recording data to disk.
                start_run();
        } else if(cmd == "timedrun") {
            if(!arg.empty()) {
                double runSeconds = strtod(arg.c_str(), nullptr);
                if(StringManipulation::IsNumeric(arg) && runSeconds > 0.0)
                    start_run(true, runSeconds);
                else
                    std::cout << sys_message_head << Display::ErrorStr() << " User attempted to run for an invalid length of time (" << arg << ")!\n";
            } else{
                std::cout << sys_message_head << "Invalid number of parameters to timedrun\n";
                std::cout << sys_message_head << " -SYNTAX- timedrun <seconds>\n";
            }
        } else if(cmd == "startacq" || cmd == "startvme") { // Tell POLL to start data acquisition without recording to disk.
            start_run(false);
        } else if(cmd == "stop" || cmd == "stopacq" || cmd == "stopvme") { // Tell POLL to stop recording data to disk and stop acq.
            stop_run();
        } else if(cmd == "shm") { // Toggle "shared-memory" mode
            if(shm_mode) {
                std::cout << sys_message_head << "Toggling shared-memory mode OFF\n";
                shm_mode = false;
            } else{
                std::cout << sys_message_head << "Toggling shared-memory mode ON\n";
                shm_mode = true;
            }
        } else if(cmd == "reboot") { // Tell POLL to attempt a PIXIE crate reboot
            if(doMcaRun_)
                std::cout << sys_message_head << "Warning! Cannot reboot while MCA is running\n";
            else if(acq_running || doMcaRun_)
                std::cout << sys_message_head << "Warning! Cannot reboot while acquisition running\n";
            else {
                do_reboot = true;
                poll_term_->pause(do_reboot);
            }
        } else if(cmd == "hup" || cmd == "spill") { // Force spill
            if(doMcaRun_)
                std::cout << sys_message_head << "Command not available for MCA run\n";
            else if(!acq_running)
                std::cout << sys_message_head << "Acquisition is not running\n";
            else
                force_spill = true;
        } else if(cmd == "fdir") { // Change the output file directory
            if (arg.empty())
                std::cout << sys_message_head << "Using output directory '" << output_directory << "'\n";
            else if (file_open)
                std::cout << sys_message_head << Display::WarningStr("Warning:") << " Directory cannot be changed while a file is open!\n";
            else {
                output_directory = arg;
                current_file_num = 0;

                // Append a '/' if the user did not include one
                if(*(output_directory.end()-1) != '/') { output_directory += '/'; }

                std::cout << sys_message_head << "Set output directory to '" << output_directory << "'.\n";

                //Check what run files already exist.
                unsigned int temp_run_num = next_run_num;
                std::string filename = output_file.GetNextFileName(next_run_num,filename_prefix, output_directory);
                if (temp_run_num != next_run_num) {
                    std::cout << sys_message_head << Display::WarningStr("Warning") << ": Run file existed for run " << temp_run_num << "! Next run number will be " << next_run_num << ".\n";
                }

                std::cout << sys_message_head << "Next file will be '" << filename << "'.\n";
            }
        } else if (cmd == "prefix") {
            if (arg.empty())
                std::cout << sys_message_head << "Using output filename prefix '" << filename_prefix << "'.\n";
            else if (file_open)
                std::cout << sys_message_head << Display::WarningStr("Warning:") << " Prefix cannot be changed while a file is open!\n";
            else {
                filename_prefix = arg;
                next_run_num = 1;

                //Check what run files already exist.
                std::string filename = output_file.GetNextFileName(next_run_num,filename_prefix, output_directory);
                if (next_run_num != 1) {
                    std::cout << sys_message_head << Display::WarningStr("Warning") << ": Some run files existed! Next run number will be " << next_run_num << ".\n";
                }

                std::cout << sys_message_head << "Set output filename prefix to '" << filename_prefix << "'.\n";
                std::cout << sys_message_head << "Next file will be '" << output_file.GetNextFileName(next_run_num,filename_prefix, output_directory) << "'.\n";
            }
        } else if(cmd == "title") { // Change the title of the output file
            if (arg.empty())
                std::cout << sys_message_head << "Using output file title '" << output_title << "'.\n";
            else if (file_open)
                std::cout << sys_message_head << Display::WarningStr("Warning:") << " Run title cannot be changed while a file is open!\n";
            else {
                //Check if argument is within double quotes and strip them. Otherwise take the whole argument.
                if (arg.find_first_of('"') == 0 && arg.find_last_of('"') == arg.length() - 1)
                    output_title = arg.substr(1,arg.length() - 2);
                else
                    output_title = arg;

                if(output_format == 0 && output_title.size() > 80) {
                    std::cout << sys_message_head << Display::WarningStr("Warning:") << " Title length " << output_title.size() - 80 << " characters too long for ldf format!\n";
                    output_title = output_title.substr(0, 80);
                }
                std::cout << sys_message_head << "Set run title to '" << output_title << "'.\n";
            }
        } else if(cmd == "runnum") { // Change the run number to the specified value
            if (arg.empty()) {
                if (output_file.IsOpen())
                    std::cout << sys_message_head << "Current output file run number '" << output_file.GetRunNumber()
                              << "'.\n";
                if (!output_file.IsOpen() || next_run_num != output_file.GetRunNumber())
                    std::cout << sys_message_head << "Next output file run number '" << next_run_num << "' for prefix '"
                              << filename_prefix << "'.\n";
            } else if (file_open) {
                std::cout << sys_message_head << Display::WarningStr("Warning:") << " Run number cannot be changed while a file is open!\n";
            } else {
                next_run_num = (unsigned int)std::stoi(arg);
                std::string filename = output_file.GetNextFileName(next_run_num,filename_prefix, output_directory);
                if (next_run_num != (unsigned int)std::stoi(arg)) {
                    std::cout << sys_message_head << Display::WarningStr("Wanring") << ": Run file existed for run " << std::stoi(arg) << ".\n";
                }
                std::cout << sys_message_head << "Set run number to '" << next_run_num << "'.\n";
                std::cout << sys_message_head << "Next file will be '" << filename << "'.\n";
            }
        } else
            std::cout << sys_message_head << "Unknown command '" << cmd << "'\n";
    }
}

void Poll::RunControl() {
    time_t acqStartTime = {0};
    time_t currentTime = {0};
    while(true) {
        if(kill_all) { // Supersedes all other commands
            if(acq_running || isMcaRunning_)
                do_stop_acq = true; // Safety catch
            else
                break;
        }

        if(do_reboot) { // Attempt to reboot the PIXIE crate
            if(acq_running)
                do_stop_acq = true; // Safety catch
            else{
                std::cout << sys_message_head << "Attempting PIXIE crate reboot\n";
                pif_->Boot(Interface::BootFlags::BootAll, false);
                printf("Press Enter key to continue...");
                std::cin.get();
                do_reboot = false;
            }
        }

        if(doMcaRun_) {
            if(acq_running)
                do_stop_acq = true;
            else {
                if(!isMcaRunning_) {
                    if(mcaRunLengthInSeconds_ > 0.0)
                        std::cout << sys_message_head << "Performing MCA data run for " << mcaRunLengthInSeconds_ << " s\n";
                    else
                        std::cout << sys_message_head << "Performing infinite MCA data run. Type \"stop\" to quit\n";

                    try {
                        mca_ = new McaRoot(pif_, mcaBasename_.c_str());
                        pif_->RemovePresetRunLength(0);
                        pif_->StartHistogramRun();
                    } catch (std::invalid_argument &invalidArgument) {
                        std::cout << sys_message_head << "Poll::RunControl::doMcaRun - Caught invalid argument while "
                                                         "initializing the MCA\n" << invalidArgument.what() << "\n";
                        doMcaRun_ = false;
                        had_error = true;
                        continue;
                    }
                    isMcaRunning_ = true;
                }

                if( (mcaRunLengthInSeconds_ != 0 && mca_->GetRunTimeInSeconds() >= mcaRunLengthInSeconds_) || do_stop_acq) {
                    pif_->EndRun();
                    std::cout << sys_message_head << "Ending MCA run.\n";
                    std::cout << sys_message_head << "Ran for " << mca_->GetRunTimeInSeconds() << " s.\n";
                    delete mca_;
                    do_stop_acq = false;
                    doMcaRun_ = false;
                    isMcaRunning_ = false;
                } else{
                    sleep(1); // Sleep for a small amount of time.
                    if(!mca_->Step()) { // Update the histograms.
                        std::cout << Display::ErrorStr("Run TERMINATED") << std::endl;
                        delete mca_;
                        doMcaRun_ = false;
                        isMcaRunning_ = false;
                        had_error = true;
                    }
                }
            }
        }//if(doMcaRun_)

        //Start acquistion
        if (do_start_acq) {
            if (!acq_running) {
                if(record_data) {
                    //Close a file if open
                    if(output_file.IsOpen()) {
                        std::cout << Display::WarningStr() << " Unexpected output file open! I'm closing it!\n";
                        CloseOutputFile();
                    }

                    //Prepare the output file
                    if (!OpenOutputFile()) {
                        do_start_acq = false;
                        acq_running = false;
                        record_data = false;
                        had_error = true;
                        continue;
                    }
                }

                //Start list mode
                if(pif_->StartListModeRun(LIST_MODE_RUN, NEW_RUN)) {
                    time(&acqStartTime);
                    if (record_data)
                        std::cout << "Run " << output_file.GetRunNumber();
                    else
                        std::cout << "Acq";
                    std::cout << " started on " << ctime(&acqStartTime);

                    acq_running = true;
                    startTime = usGetTime(0);
                    lastSpillTime = 0;
                }
                else{
                    std::cout << sys_message_head << "Failed to start list mode run. Try rebooting PIXIE\n";
                    acq_running = false;
                    had_error = true;
                }
                do_start_acq = false;
            } //if(!acq_running)
            else  {
                std::cout << sys_message_head << "Already running!\n";
                do_start_acq = false;
            }
        }

        if(acq_running) {
            // Check the run time.
            time(&currentTime);

            if(runTime > 0.0 && difftime(currentTime, acqStartTime) >= runTime)
                stop_run(); // Handle this cleanly.

            //Handle a stop signal
            if(do_stop_acq) {
                // Read data from the modules.
                if (!had_error) ReadFIFO();

                // Instruct all modules to end the current run.
                pif_->EndRun();

                // Check if each module has ended its run properly.
                for(size_t mod = 0; mod < n_cards; mod++) {
                    //If the run status is 1 then the run has not finished in the module.
                    // We need to read it out.
                    if(pif_->CheckRunStatus((short)mod) == 1) {
                        if (!is_quiet) std::cout << "Module " << mod << " still has " << pif_->CheckFIFOWords((short)mod) << " words in the FIFO.\n";
                        //We set force_spill to true in case the remaining words is small.
                        force_spill = true;
                        //We sleep to allow the module to finish.
                        sleep(1);
                        //We read the FIFO out.
                        if (!had_error) ReadFIFO();
                    }

                    //Print the module status.
                    std::stringstream leader;
                    leader << "Run end status in module " << mod;
                    if (!partialEvents[mod].empty()) {
                        ///\bug Warning Str colors oversets the number of characters.
                        leader << Display::WarningStr(" (partial evt)");
                        partialEvents[mod].clear();
                    }

                    Display::LeaderPrint(leader.str());
                    if(!pif_->CheckRunStatus((short)mod))
                        std::cout << Display::OkayStr() << std::endl;
                    else {
                        std::cout << Display::ErrorStr() << std::endl;
                        had_error = true;
                    }
                }

                if (record_data)
                    std::cout << "Run " << output_file.GetRunNumber();
                else
                    std::cout << "Acq";
                std::cout << " stopped on " << ctime(&currentTime);

                statsHandler->ClearRates();
                statsHandler->Dump();
                statsHandler->ClearTotals();

                //Close the output file
                if(output_file.IsOpen()) CloseOutputFile();

                //Reset status flags
                do_stop_acq = false;
                acq_running = false;
            } //if (do_stop_acq) -- End of handling a stop acq flag

            // Read data from the modules.
            ReadFIFO();
        }

        UpdateStatus();

        //Sleep the run control if idle to reduce CPU utilization.
        if (!acq_running && !doMcaRun_)
            sleep(1);
    }

    run_ctrl_exit = true;
    std::cout << "Run Control exited\n";
}

void Poll::UpdateStatus() {
    //Build status string
    std::stringstream status;
    if (had_error)
        status << Display::ErrorStr("[ERROR]");
    else if (acq_running && record_data)
        status << Display::OkayStr("[ACQ]");
    else if (acq_running && !record_data)
        status << Display::WarningStr("[ACQ]");
    else if (doMcaRun_)
        status << Display::OkayStr("[MCA]");
    else
        status << Display::InfoStr("[IDLE]");

    if (file_open)
        status << " Run " << output_file.GetRunNumber();

    if(doMcaRun_) {
        status << " " << (int)mca_->GetRunTimeInSeconds() << "s";
        status << " of " << mcaRunLengthInSeconds_ << "s";
    } else{
        //Add run time to status
        status << " " << (long long) statsHandler->GetTotalTime() << "s";
        //Add data rate to status
        status << " " << StringManipulation::FormatHumanReadableSizes(statsHandler->GetTotalDataRate()) << "/s";
    }

    if (file_open) {
        if (acq_running && !record_data)
            status << TermColors::DkYellow;
        //Add file size to status
        status << " " << StringManipulation::FormatHumanReadableSizes(output_file.GetFilesize());
        status << " " << output_file.GetCurrentFilename();
        if (acq_running && !record_data)
            status << TermColors::Reset;
    }

    //Update the status bar
    poll_term_->SetStatus(status.str());
}


void Poll::ReadScalers() {
    static std::vector< std::pair<double, double> > xiaRates(16, std::make_pair<double, double>(0,0));
    static int numChPerMod = pif_->GetConfiguration().GetNumberOfChannels();

    for (unsigned short mod=0;mod < n_cards; mod++) {
        //Tell interface to get stats data from the modules.
        pif_->GetStatistics(mod);

        for (int ch=0;ch< numChPerMod; ch++)
            xiaRates[ch] = std::make_pair<double, double>(pif_->GetInputCountRate(mod, ch),pif_->GetOutputCountRate(mod,ch));

        //Populate Stats Handler with ICR and OCR.
        statsHandler->SetXiaRates(mod, &xiaRates);
    }
}
bool Poll::ReadFIFO() {
    static auto *fifoData = new Pixie16::word_t[(EXTERNAL_FIFO_LENGTH + 2) * n_cards];

    if (!acq_running)
        return false;

    //Number of words in the FIFO of each module.
    std::vector<Pixie16::word_t> nWords(n_cards);
    //Iterator to determine which card has the most words.
    std::vector<Pixie16::word_t>::iterator maxWords;

    //We loop until the FIFO has reached the threshold for any module unless we are stopping and then we skip the loop.
    for (unsigned int timeout = 0; timeout < POLL_TRIES; timeout++) {
        //Check the FIFO size for every module
        for (unsigned short mod=0; mod < n_cards; mod++)
            nWords[mod] = (unsigned int)pif_->CheckFIFOWords(mod);

        //Find the maximum module
        maxWords = std::max_element(nWords.begin(), nWords.end());
        if(*maxWords > threshWords)
            break;
    }

    //We need to read the data out of the FIFO
    if (*maxWords > threshWords || force_spill) {
        force_spill = false;
        //Number of data words read from the FIFO
        size_t dataWords = 0;

        //Loop over each module's FIFO
        for (unsigned short mod=0;mod < n_cards; mod++) {

            //if the module has no words in the FIFO we continue to the next module
            if (nWords[mod] < MIN_FIFO_READ) {
                // write an empty buffer if there is no data
                fifoData[dataWords++] = 2;
                fifoData[dataWords++] = mod;
                continue;
            } else if (nWords[mod] < 0) {
                std::cout << Display::WarningStr("Number of FIFO words less than 0") << " in module " << mod << std::endl;
                // write an empty buffer if there is no data
                fifoData[dataWords++] = 2;
                fifoData[dataWords++] = mod;
                continue;
            }

            //Check if the FIFO is overfilled
            bool fullFIFO = (nWords[mod] >= EXTERNAL_FIFO_LENGTH);
            if (fullFIFO) {
                std::cout << Display::ErrorStr() << " Full FIFO in module " << mod
                          << " size: " << nWords[mod] << "/"
                          << EXTERNAL_FIFO_LENGTH << Display::ErrorStr(" ABORTING!") << std::endl;
                had_error = true;
                do_stop_acq = true;
                return false;
            }

            //We inject two words describing the size of the FIFO spill and the module.
            //We inject the size after it has been computed so we skip it for now and only add the module number.
            dataWords++;
            fifoData[dataWords++] = mod;

            //We store the partial event if we had one
            for (size_t i=0;i<partialEvents[mod].size();i++)
                fifoData[dataWords + i] = partialEvents[mod].at(i);

            //Try to read FIFO and catch errors.
            if(!pif_->ReadFIFOWords(&fifoData[dataWords + partialEvents[mod].size()], nWords[mod], mod, debug_mode)) {
                std::cout << Display::ErrorStr() << " Unable to read " << nWords[mod] << " from module " << mod << "\n";
                had_error = true;
                do_stop_acq = true;
                return false;
            }

            //Print a message about what we did
            if(!is_quiet || debug_mode) {
                std::cout << "Read " << nWords[mod] << " words from module " << mod;
                if (!partialEvents[mod].empty())
                    std::cout << " and stored " << partialEvents[mod].size() << " partial event words";
                std::cout << " to buffer position " << dataWords << std::endl;
            }

            //After reading the FIFO and printing a status message we can update the number of words to include the partial event.
            nWords[mod] += partialEvents[mod].size();
            //Clear the partial event
            partialEvents[mod].clear();

            //We now need to parse the event to determine if there is a hanging event. Also, allows a check for corrupted data.
            size_t parseWords = dataWords;
            //We declare the eventSize outside the loop in case there is a partial event.
            Pixie16::word_t eventSize = 0, prevEventSize = 0;

            ///@TODO Eventually we'll need to make sure that we get the correct crate number here.
            Pixie16::word_t slotExpected = pif_->GetConfiguration().GetSlotNumber(0, mod);

            while (parseWords < dataWords + nWords[mod]) {
                //Check first word to see if data makes sense.
                // We check the slot, channel and event size.
                Pixie16::word_t slotRead = ((fifoData[parseWords] & 0xF0) >> 4);
                Pixie16::word_t chanRead = (fifoData[parseWords] & 0xF);
                eventSize = ((fifoData[parseWords] & 0x7FFE2000) >> 17);
                bool virtualChannel = ((fifoData[parseWords] & 0x20000000) != 0);

                if( slotRead != slotExpected ) {
                    std::cout << Display::ErrorStr() << " Slot read " << slotRead
                              << " not the same as slot expected "
                              << slotExpected << std::endl;
                    had_error = true;
                }
                if (chanRead < 0 || chanRead > 15) {
                    std::cout << Display::ErrorStr() << " Channel read (" << chanRead << ") not valid!\n";
                    had_error = true;
                }
                if(eventSize == 0) {
                    std::cout << Display::ErrorStr() << " ZERO EVENT SIZE in mod " << mod << "!\n";
                    had_error = true;
                }

                if (had_error)
                    break;

                // Update the statsHandler with the event (for monitor.bash)
                if(!virtualChannel && statsHandler)
                    statsHandler->AddEvent(mod, chanRead, sizeof(Pixie16::word_t) * eventSize);

                //Iterate to the next event and continue parsing
                parseWords += eventSize;
                prevEventSize = eventSize;
            }

            //We now check the outcome of the data parsing.
            //If we have too many words as an event was not completely pulled form the FIFO
            if (parseWords > dataWords + nWords[mod]) {
                auto missingWords = Pixie16::word_t(parseWords - dataWords - nWords[mod]);
                Pixie16::word_t partialSize = eventSize - missingWords;
                if (debug_mode)
                    std::cout << "Partial event " << partialSize << "/" << eventSize << " words!\n";

                //We could get the words now from the FIFO, but me may have to wait. Instead we store the partial event for the next FIFO read.
                for(unsigned short i=0;i< partialSize;i++)
                    partialEvents[mod].push_back(fifoData[parseWords - eventSize + i]);

                //Update the number of words to indicate removal or partial event.
                nWords[mod] -= partialSize;
            } else if (parseWords < dataWords + nWords[mod]) { //If parseWords is small then the parse failed for some reason
                //Determine the fifo position from successfully parsed words plus the last event length.
                std::cout << Display::ErrorStr() << " Parsing indicated corrupted data for module " << mod << ".\n";
                std::cout << "| Parsing failed at " << parseWords - dataWords << "/" << nWords[mod]
                          << " (" << parseWords << "/" << dataWords  + nWords[mod] << ") words into FIFO." << std::endl;

                //Print the previous event
                std::cout << "|\n| Event prior to parsing error (" << prevEventSize << " words):";
                std::cout << std::hex << std::setfill('1');
                for(size_t i=0;i< prevEventSize;i++) {
                    if (i%5 == 0)
                        std::cout << std::endl << "|  ";
                    std::cout << "0x" << std::right << std::setw(8) << std::setfill('0');
                    std::cout << fifoData[parseWords - prevEventSize + i] << " ";
                }
                std::cout << std::dec << std::setfill(' ') << std::endl;

                //Print the parsed event
                std::cout << "|\n| Event at parsing error (" << eventSize << " words):";
                size_t outputSize = eventSize;
                if (eventSize > 50) {
                    outputSize = 50;
                    std::cout << "\n| (Truncated at " << outputSize << " words.)";
                }
                std::cout << std::hex << std::setfill('0');
                for(size_t i=0;i< outputSize;i++) {
                    if (i%5 == 0)
                        std::cout << std::endl << "|  ";
                    std::cout << "0x" << std::right << std::setw(8) << std::setfill('0');
                    std::cout << fifoData[parseWords + i] << " ";
                }
                std::cout << std::dec << std::setfill(' ') << std::endl;

                //Print the following event
                //Determine size of following event.
                Pixie16::word_t nextEventSize = 0;
                if (parseWords + eventSize < dataWords + nWords[mod]) {
                    nextEventSize = ((fifoData[parseWords + eventSize] & 0x7FFE2000) >> 17);
                }
                std::cout << "|\n| Event after parsing error (" << nextEventSize << " words):";

                //Determine output size for event.
                outputSize = nextEventSize;
                if (eventSize > 50)
                    outputSize = 50;
                if (parseWords + eventSize + outputSize >= dataWords + nWords[mod])
                    outputSize = dataWords + nWords[mod] - (parseWords + eventSize);
                if (outputSize != nextEventSize)
                    std::cout << "\n| (Truncated at " << outputSize << " words.)";

                std::cout << std::hex << std::setfill('0');
                for(size_t i=0;i< outputSize;i++) {
                    if (i%5 == 0)
                        std::cout << std::endl << "|  ";
                    std::cout << "0x" << std::right << std::setw(8);
                    std::cout << fifoData[parseWords + eventSize + i] << " ";
                }
                std::cout << std::dec << std::setfill(' ') << std::endl << "|\n";

                do_stop_acq = true;
                had_error = true;
                return false;
            }

            //Assign the first injected word of spill to final spill length
            fifoData[dataWords - 2] = nWords[mod] + 2;
            //The data should be good so we iterate the position in the storage array.
            dataWords += nWords[mod];
        } //End loop over modules for reading FIFO

        //Get the length of the spill
        double spillTime = usGetTime(startTime);
        double durSpill = spillTime - lastSpillTime;
        lastSpillTime = spillTime;

        // Add time to the statsHandler and check if interval has been exceeded.
        //If exceed interval we read the scalers from the modules and dump the stats.
        if (statsHandler->AddTime(durSpill * 1e-6)) {
            ReadScalers();
            statsHandler->Dump();
            statsHandler->ClearRates();
        }

        if (!is_quiet || debug_mode)
            std::cout << "Writing/Broadcasting " << dataWords << " words.\n";
        //We have read the FIFO now we write the data
        if (record_data)
            write_data(fifoData, (unsigned int)dataWords);
        broadcast_data(fifoData, (unsigned int)dataWords);

    } //If we had exceeded the threshold or forced a flush

    return true;
}