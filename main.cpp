#include <iostream>
#include <iomanip>
#include <unordered_map>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include <cmath>
#include <sstream>

// Packet processing libs
#include <ifaddrs.h>
#include <netinet/in.h>
#include <tins/tins.h>
#include <tins/constants.h>

// Mail libs
#include <message.hpp>
#include <smtp.hpp>

//using namespace std;
using std::cout;
using std::endl;
using std::string;
using std::unordered_map;

using namespace Tins;

using mailio::message;
using mailio::mail_address;
using mailio::smtps;
using mailio::smtp_error;
using mailio::dialog_error;

#define CAP_LEN 140 // How many bytes to capture
#define THOUSAND 1000
#define MILLION 1000000
#define RTT_NUM_SAMPLES 180 // Keep the last n samples
#define EMAIL_INTERVAL 600 // Min break (in secs) between e-mails if threshold is crossed

volatile sig_atomic_t gINTERRUPTED = false; // Has program caught any OS signals

const bool sendEmail = true; // Control whether or send email or not (currently hard-coded)
                             // TODO: Make this configurable via command line

static void signalHandler(int sigVal) {
    printf("\n\nSignal caught!\n\n");
    gINTERRUPTED = true;
}

typedef uint32_t ICMP_ID;

inline ICMP_ID GenICMP_ID(const uint16_t id, const uint16_t seq_no) {
    return (id << 16) | seq_no;
}

double RTTAvg(const std::vector<double>& vTimes) {
    double sum = 0;
    for (auto i : vTimes)
        sum += i;

    return sum / vTimes.size();
}

// Sample (not population) variance
double RTTVar(const std::vector<double>& vTimes, const double rttAvg) {
    double sum = 0, diff = 0;
    for (auto i : vTimes) {
        diff = i - rttAvg;
        sum += (diff * diff);
    }

    if (vTimes.size() > 1)
        return sum / (vTimes.size() - 1);
    else
        return 0; // Can't divide by 0, so this is undefined
}

/* Updates vTimes structure given the new value newVal
 *
 * sampleAvg and sampleVar contains current avg and variance.
 * The function will calculate updated avg and variance values, and
 * update the sampleAvg and sampleVar parameters.
 */
void UpdateStats(std::vector<double>& vTimes, const double newVal,
                    double& sampleAvg, double& sampleVar) {

    vTimes.push_back(newVal);
    if (vTimes.size() > RTT_NUM_SAMPLES) {
        double oldestVal = vTimes.front();
        double oldAvg = sampleAvg;

        vTimes.erase(vTimes.begin()); // Keep it bounded
        sampleAvg += (newVal - oldestVal) / RTT_NUM_SAMPLES;
        sampleVar += (newVal - oldestVal) * (newVal - sampleAvg + oldestVal - oldAvg) / (RTT_NUM_SAMPLES - 1);
    } else {
        /* If pre-insertion size is less than RTT_NUM_SAMPLES, then
         * we shouldn't use simplified rolling update formulas. Do
         * full calculations from scratch. */
        sampleAvg = RTTAvg(vTimes);
        sampleVar = RTTVar(vTimes, sampleAvg);
    }

    return;
}

void SendEmail(const string& mailSubject, const string& mailBody) {
    // Emails don't accept simple \n new-lines
    // Must replace them with \r\n
    string fixedMailBody = boost::replace_all_copy(mailBody, "\n", "\r\n");

    try {
        // Create e-mail message
        message msg;
        msg.sender(mail_address("RTT-Mon", "sender@someemail.com")); // Replace w/ your your email account
        msg.add_recipient(mail_address("Receiver's Email", "receiver@someemail.com")); // Replace w/ intended receiver
        msg.subject(mailSubject);
        msg.content(fixedMailBody);

        // Connect to server
        smtps conn("smtp.gmail.com", 587);
        char password[9] = {'P', 'A', 'S', 'S', 'W', 'O', 'R', 'D', '\0'};
        conn.authenticate("sender@someemail.com", password, smtps::auth_method_t::START_TLS); // Replace w/ your email account
        conn.submit(msg);
    }
    catch (smtp_error& exc) {
        cout << "SMTP Error:\n" << exc.what() << endl;
    }
    catch (dialog_error& exc) {
        cout << "Dialog Error:\n" << exc.what() << endl;
    }

    return;
}

int main(int argc, char *argv[]) {
    // Set up signal catching
    struct sigaction action;
    action.sa_handler = signalHandler;
    action.sa_flags = 0;
    sigemptyset (&action.sa_mask);
    sigaction (SIGINT, &action, NULL);
    sigaction (SIGTERM, &action, NULL);

    string destIP;
    if (argc != 2) {
        cout << "Usage: ./rtt-mon <destination IP>" << endl;
        exit(0);
    } else {
        destIP = argv[1];
    }

    // The address to resolve
    // The interface we'll use, since we need the sender's HW address
    NetworkInterface outIface("eth0");
    // The interface's information
    auto info = outIface.addresses();

    /* Create IP packet w/ ICMP */
    IP ipPkt = IP(destIP, info.ip_addr) / ICMP(); // Default type is echo request
    ipPkt.ttl(64);
    ipPkt.protocol(Constants::IP::PROTO_ICMP);

    // Set ICMP to echo request
    ICMP &icmp = ipPkt.rfind_pdu<ICMP>();
    icmp.set_echo_request(1234, 1);

    /* ICMP ID and timing-related variables */
    unordered_map<ICMP_ID, Timestamp> icmpMap;
    ICMP_ID icmpID = GenICMP_ID(1234, 0); // Simply 1234 l-shifted 16 bits
    Timestamp now;
    struct timeval elapsed;

    /* RTT-related variables and data structures */
    std::vector<double> elapsedTimes;
    double rtt = 0, rttAvg = 0, rttVar = 0;

    /* Email-related variables */
    Timestamp lastEmail;
    std::stringstream emailSubject, emailBody;

    cout << "Calculating stats over a sliding window of " << RTT_NUM_SAMPLES << " samples" << endl;

    // The sender
    PacketSender sender;
    for (uint16_t i = 1; !gINTERRUPTED; i++, sleep(1)) {
        icmp.sequence(i); // Update seq #
        icmpMap[icmpID + i] = Timestamp::current_time();

        // Send and receive the response.
        std::unique_ptr<PDU> response(sender.send_recv(ipPkt, outIface));
        // Did we receive anything?
        if (response) {
            response->rfind_pdu<ICMP>(); // Will throw exception if not ICMP

            now = now.current_time();
            Timestamp& sentTime = icmpMap[icmpID + i]; // TODO: Should actually instantiate new ICMP_ID
                                                       //       Assumes synchronicity
            elapsed.tv_sec = now.seconds() - sentTime.seconds();
            if (now.microseconds() < sentTime.microseconds()) {
                elapsed.tv_sec--;
                elapsed.tv_usec = MILLION - sentTime.microseconds() + now.microseconds();
            } else {
                elapsed.tv_usec = now.microseconds() - sentTime.microseconds();
            }

            icmpMap.erase(icmpID + i); // TODO: Same note as above

            rtt = (double)((elapsed.tv_sec * MILLION) + elapsed.tv_usec) / THOUSAND;
            UpdateStats(elapsedTimes, rtt, rttAvg, rttVar);
            cout << std::fixed << std::setprecision(3) << "RTT: " << rtt << " ms\t" <<
                "; Avg: " << rttAvg <<
                "\t; Stdev: " << sqrt(rttVar) << endl;

            // Decide if an e-mail should be sent
            if (sendEmail) {
                emailSubject.str("");
                emailBody.str("");

                if (rttAvg > 5 && elapsedTimes.size() == RTT_NUM_SAMPLES &&
                        (lastEmail.current_time().seconds() - lastEmail.seconds()) > EMAIL_INTERVAL) {
                    cout << "Sending email... threshold over 5" << endl;
                    emailSubject << "RTT-Mon to " << destIP << ": Warning (RTT Avg: " << rttAvg << " ms)";
                    emailBody << "Sample stdev is: " << sqrt(rttVar) << endl << endl;
                    emailBody << "Last " << RTT_NUM_SAMPLES << " samples (latest at bottom):";
                    for (auto i : elapsedTimes)
                        emailBody << "RTT: " << i << " ms" << endl;

                    SendEmail(emailSubject.str(), emailBody.str());
                    lastEmail = lastEmail.current_time();
                }
            }
        } else {
            cout << "No response?!" << endl;
        }
    }

    cout << "Ending..." << endl;


    return 0;
}
