//
// Created by Phillip Romig on 7/16/24.
//
#include <cstdint>
#include <cstdlib>
#include <ios>
#include <iostream>
#include <fstream>
#include <sys/socket.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <array>
#include <vector>

#include "datagram.h"
#include "timerC.h"
#include "unreliableTransport.h"
#include "logging.h"


#define WINDOW_SIZE 10
#define TIMER_DURATION 20 // 20 for loopback testing - will want to bump this up for isen

int main(int argc, char* argv[]) {

    // Defaults
    uint16_t portNum(12345);
    std::string hostname("");
    std::string inputFilename("");
    int requiredArgumentCount(0);


    int opt;
    try {
        while ((opt = getopt(argc, argv, "f:h:p:d:")) != -1) {
            switch (opt) {
                case 'p':
                    portNum = std::stoi(optarg);
                    break;
                case 'h':
                    hostname = optarg;
		    requiredArgumentCount++;
                    break;
                case 'd':
                    LOG_LEVEL = std::stoi(optarg);
                    break;
                case 'f':
                    inputFilename = optarg;
		    requiredArgumentCount++;
                    break;
                case '?':
                default:
                    std::cout << "Usage: " << argv[0] << " -f filename -h hostname [-p port] [-d debug_level]" << std::endl;
                    break;
            }
        }
    } catch (std::exception &e) {
        std::cout << "Usage: " << argv[0] << " -f filename -h hostname [-p port] [-d debug_level]" << std::endl;
        FATAL << "Invalid command line arguments: " << e.what() << ENDL;
        return(-1);
    }

    if (requiredArgumentCount != 2) {
        std::cout << "Usage: " << argv[0] << " -f filename -h hostname [-p port] [-d debug_level]" << std::endl;
	std::cerr << "hostname and filename are required." << std::endl;
	return(-1);
    }


    TRACE << "Command line arguments parsed." << ENDL;
    TRACE << "\tServername: " << hostname << ENDL;
    TRACE << "\tPort number: " << portNum << ENDL;
    TRACE << "\tDebug Level: " << LOG_LEVEL << ENDL;
    TRACE << "\tOutput file name: " << inputFilename << ENDL;

    // *********************************
    // * Open the input file
    // *********************************
    std::ifstream in(inputFilename, std::ios::binary);
    if(!in.is_open()) {
        FATAL << "Could not open file: " << inputFilename << ENDL;
        exit(EXIT_FAILURE);
    }

    try {

        // ***************************************************************
        // * Initialize your timer, window and the unreliableTransport etc.
        // **************************************************************
        unreliableTransportC udt(hostname, portNum);
        timerC rto(TIMER_DURATION);

        uint16_t base = 1; // first unack'd sequence #
        uint16_t nextSeqNum = 1; // to assign to next outbound packet.

        // Reminder to index this with modulo! (seqNum % WINDOW_SIZE)
        std::vector<datagramS> window(WINDOW_SIZE);

        bool fileEOF = false;


        // ***************************************************************
        // * Send the file one datagram at a time until they have all been
        // * acknowledged
        // **************************************************************
        // bool allSent(false);
        // bool allAcked(false);
        // while ((!allSent) && (!allAcked)) {

        // Main loop runs until:
        // - we’ve finished reading the file (fileEOF == true), AND
        // - every sent packet up to nextSeqNum - 1 has been acknowledged (base == nextSeqNum).
        while (!fileEOF || base < nextSeqNum) {	

            // While there is space in the window (nextSeqNum < base + WINDOW_SIZE) and we still have file data:
            // build packets from file chunks and send them.
            while(!fileEOF && nextSeqNum < base + WINDOW_SIZE) {
                datagramS pkt{};

                // Note: read() does not return a count; gcount() tells you how many bytes were actually read.
                in.read(pkt.data, MAX_PAYLOAD_LENGTH);
                std::streamsize bytesReadFromFile = in.gcount();

                if(bytesReadFromFile <= 0) {
                    fileEOF = true;
                    break;
                }

                pkt.seqNum = nextSeqNum;
                pkt.ackNum = 0;

                pkt.payloadLength = static_cast<uint8_t>(bytesReadFromFile);

                pkt.checksum = computeChecksum(pkt);

                window[nextSeqNum % WINDOW_SIZE] = pkt;

                udt.udt_send(pkt);

                // If this packet becomes the new base (i.e., we had no unacked packets), start the retransmission timer.
                // In no-loss mode, this timer (hopefully lol) won’t expire, but keeping the logic here makes it easy to turn on loss later.
                if (base == nextSeqNum) {
                    rto.start();
                }

                nextSeqNum++;
            }

            // udt_receive is non-blocking. This loop is for when we have a queue of acks to process.
            // if we get a 0 read, then we polled too fast and we can just wait until the next cycle of the outer loop.
            while(true) {
                datagramS ack{};
                ssize_t r = udt.udt_receive(ack);

                if(r <= 0) break; // no ACKs available rn. Wait until next round of outer loop.

                // Ignore corrupt ack
                if(!validateChecksum(ack)) {
                    continue;
                }

                uint16_t newBase = static_cast<uint16_t>(ack.ackNum + 1);

                // Slide window forward iff this ack advances base
                if(newBase > base) {
                    base = newBase;
                    if(base == nextSeqNum) {
                        rto.stop(); // no packets in flight.
                    } else {
                        rto.start(); // restart timer for new base.
                    }
                }

                // implicitly ignoring duplicate (old #) ACKs
            }
 
            // Check to see if the timer has expired.
            if(rto.timeout()) {
                WARNING << "TIMEOUT OF RTO: Retransmitting Window Starting At " << base << ENDL;

                // Retransmit window
                for(uint16_t s = base; s < nextSeqNum; s++) {
                    udt.udt_send(window[s % WINDOW_SIZE]);
                }

                // Try timer again.
                rto.start();
            }
        }

        // cleanup and close the file and network.

        // Sending an empty payload to indicate end.
        // We don't receive an ACK/Signal from receiver that they've seen this. So consider sending this a few times in case of loss?
        datagramS fin{};
        fin.seqNum = nextSeqNum;
        fin.ackNum = 0;
        fin.payloadLength = 0;
        fin.checksum = computeChecksum(fin);
        udt.udt_send(fin);

        // How do I close the file? Will it close if we leave scope anyways?

    } catch (std::exception &e) {
        FATAL<< "Error: " << e.what() << ENDL;
        exit(1);
    }
    return 0;
}
