#ifndef __OPTIONS_H
#define __OPTIONS_H

#include <iostream>
#include <regex>
#include <sys/stat.h>
#include <upcxx/upcxx.hpp>

#include "version.h"

#include "CLI11.hpp"

using std::cout;
using std::endl;
using std::vector;

#include "upcxx_utils/log.hpp"
#include "upcxx_utils/timers.hpp"

using namespace upcxx_utils;


#define YES_NO(X) ((X) ? "YES" : "NO")

class Options {
  vector<string> splitter(string in_pattern, string& content) {
    vector<string> split_content;
    std::regex pattern(in_pattern);
    copy(std::sregex_token_iterator(content.begin(), content.end(), pattern, -1),
         std::sregex_token_iterator(), back_inserter(split_content));
    return split_content;
  }

  template <typename T>
  string vec_to_str(const vector<T> &vec, const string &delimiter=",") {
    std::ostringstream oss;
    for (auto elem : vec) {
      oss << elem;
      if (elem != vec.back()) oss << delimiter;
    }
    return oss.str();
  }

  void setup_output_dir() {
    if (!upcxx::rank_me()) {
      // create the output directory and stripe it 
	  if (mkdir(output_dir.c_str(), S_IRWXU) == -1) {
		// could not create the directory
		if (errno == EEXIST) {
		  cerr << KLRED << "WARNING: " << KNORM << "Output directory " << output_dir
			   << " already exists. May overwrite existing files\n";
		} else {
		  ostringstream oss;
		  oss << KLRED << "ERROR: " << KNORM  << " Could not create output directory " << output_dir
			  << ": " << strerror(errno) << endl;
		  throw std::runtime_error(oss.str());
		}
	  } else {
		// created the directory - now stripe it if possible
		auto status = std::system("which lfs 2>&1 > /dev/null");
		if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
		  string cmd = "lfs setstripe -c -1 " + output_dir;
		  auto status = std::system(cmd.c_str());
		  if (WIFEXITED(status) && WEXITSTATUS(status) == 0) cout << "Set Lustre striping on the output directory\n";
		  else cout << "Failed to set Lustre striping on output directory: " << WEXITSTATUS(status) << endl;
		}
	  }
    }
    upcxx::barrier();
    // all change to the output directory
    if (chdir(output_dir.c_str()) == -1 && !upcxx::rank_me()) {
      ostringstream oss;
      oss << KLRED << "Cannot change to output directory " << output_dir << ": " << strerror(errno) << KNORM << endl;
      throw std::runtime_error(oss.str());
    }
    upcxx::barrier();
  }

  void setup_log_file() {
    if (!upcxx::rank_me()) {
      // check to see if mhmxx.log exists. If so, rename it
      if (file_exists("mhmxx.log")) {
        string new_log_fname = "simcov-" + get_current_time(true) + ".log";
        cerr << KLRED << "WARNING: " << KNORM << output_dir << "/simcov.log exists. Renaming to " << output_dir << "/"
             << new_log_fname << endl;
        if (rename("simcov.log", new_log_fname.c_str()) == -1) DIE("Could not rename simcov.log: ", strerror(errno));
      }
    }
    upcxx::barrier();
  }

public:

  int64_t x_size = 100, y_size = 100, z_size = 100;
  int num_iters = 10;
  int num_tcells = 100;
  int num_infections = 1;
  string output_dir = "simcov-run-n" + to_string(upcxx::rank_n()) + "-N" +
    to_string(upcxx::rank_n() / upcxx::local_team().rank_n()) + "-" + get_current_time(true);
  bool show_progress = false;
  bool verbose = false;

  bool load(int argc, char **argv) {
    //SIMCOV version v0.1-a0decc6-master (Release) built on 2020-04-08T22:15:40 with g++
    string full_version_str = "SimCov version " + string(SIMCOV_VERSION) + "-" + string(SIMCOV_BRANCH) + " built on " +
	  string(SIMCOV_BUILD_DATE);
    CLI::App app(full_version_str);
    app.add_option("-x,--xdim", x_size, "X dimension")
	  ->capture_default_str();
    app.add_option("-y,--ydim", y_size, "Y dimension")
	  ->capture_default_str();
    app.add_option("-z,--zdim", z_size, "Z dimension")
	  ->capture_default_str();
    app.add_option("-i,--iterations", num_iters, "Number of iterations")
	  ->capture_default_str() ->check(CLI::Range(1, 1000000));
    app.add_option("-f,--infections", num_infections, "Number of starting infections")
	  ->capture_default_str() ->check(CLI::Range(1, 1000));
    app.add_option("-t,--tcells", num_tcells, "Number of t-cells")
	  ->capture_default_str();
    auto *output_dir_opt = app.add_option("-o,--output", output_dir, "Output directory")
                                          ->capture_default_str();
    app.add_flag("--progress", show_progress,
                 "Show progress")
                 ->capture_default_str();
    app.add_flag("-v, --verbose", verbose,
                 "Verbose output")
                 ->capture_default_str();

    auto *cfg_opt = app.set_config("--config", "simcov.config", "Load options from a configuration file");

    try {
      app.parse(argc, argv);
    } catch(const CLI::ParseError &e) {
      if (upcxx::rank_me() == 0) app.exit(e);
      return false;
    }

    upcxx::barrier();

    if (!*output_dir_opt) {
      output_dir = "simcov-run-n" + to_string(upcxx::rank_n()) + "-N" + to_string(upcxx::rank_n() / upcxx::local_team().rank_n()) +
		"-" + get_current_time(true);
      output_dir_opt->default_val(output_dir);
    }

    setup_output_dir();
    setup_log_file();

    init_logger("simcov.log", verbose);

#ifdef DEBUG
    open_dbg("debug");
#endif

    SLOG(KLBLUE, "SimCov version ", full_version_str, KNORM, "\n");

    if (upcxx::rank_me() == 0) {
      // print out all compiler definitions
      SLOG_VERBOSE(KLBLUE, "_________________________", KNORM, "\n");
      SLOG_VERBOSE(KLBLUE, "Compiler definitions:", KNORM, "\n");
      std::istringstream all_defs_ss(ALL_DEFNS);
      vector<string> all_defs((std::istream_iterator<string>(all_defs_ss)), std::istream_iterator<string>());
      for (auto &def : all_defs) SLOG_VERBOSE("  ", def, "\n");
      SLOG_VERBOSE("_________________________\n");
      SLOG(KLBLUE, "Options:", KNORM, "\n");
      auto all_opts_strings = app.config_to_str_vector(true, false);
      for (auto &opt_str : all_opts_strings) SLOG(KLBLUE, opt_str, KNORM, "\n");
      SLOG(KLBLUE, "_________________________", KNORM, "\n");
    }
    auto num_nodes = upcxx::rank_n() / upcxx::local_team().rank_n();
    SLOG("Starting run with ", upcxx::rank_n(), " processes on ", num_nodes, " node", (num_nodes > 1 ? "s" : ""), " at ",
         get_current_time(), "\n");
#ifdef DEBUG
    SWARN("Running low-performance debug mode");
#endif
    if (!upcxx::rank_me()) {
      // write out configuration file for restarts
      ofstream ofs("simcov.config");
      ofs << app.config_to_str(true, true);
    }
    upcxx::barrier();
    return true;
  }
};


#endif