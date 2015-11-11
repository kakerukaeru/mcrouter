/*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */
#include <iostream>

#include <glog/logging.h>

#include <boost/program_options.hpp>
#include <boost/regex.hpp>

#include <folly/Format.h>
#include <folly/io/async/EventBase.h>

#include "mcrouter/lib/mc/msg.h"

#include "mcrouter/tools/mcpiper/AnsiColorCodeStream.h"
#include "mcrouter/tools/mcpiper/Color.h"
#include "mcrouter/tools/mcpiper/Config.h"
#include "mcrouter/tools/mcpiper/FifoReader.h"
#include "mcrouter/tools/mcpiper/ParserMap.h"
#include "mcrouter/tools/mcpiper/PrettyFormat.h"
#include "mcrouter/tools/mcpiper/StyledString.h"
#include "mcrouter/tools/mcpiper/Util.h"
#include "mcrouter/tools/mcpiper/ValueFormatter.h"

using namespace facebook::memcache;

namespace {

struct Settings {
  // Positional args
  std::string matchExpression;

  // Named args
  std::string fifoRoot{getDefaultFifoRoot()};
  bool quiet{false};
  std::string filenamePattern;
};

// Constants
const PrettyFormat kFormat{}; // Default constructor = default coloring.

// Globals
AnsiColorCodeStream gTargetOut(std::cout);
std::unique_ptr<boost::regex> gDataPattern;
std::unique_ptr<ValueFormatter> gValueFormatter = createValueFormatter();
Settings gSettings;

std::string getUsage(const char* binaryName) {
  return folly::sformat(
      "Usage: {} [OPTION]... [PATTERN]\n"
      "Search for PATTERN in each mcrouter debug fifo in FIFO_ROOT "
      "(see options list) directory.\n"
      "If PATTERN is not provided, match everything.\n"
      "PATTERN is, by default, a basic regular expression (BRE).\n"
      , binaryName);
}

Settings parseOptions(int argc, char **argv) {
  Settings settings;

  namespace po = boost::program_options;

  // Named options
  po::options_description namedOpts("Allowed options");
  namedOpts.add_options()
    ("help,h", "Print this help message.")
    ("fifo-root,f",
      po::value<std::string>(&settings.fifoRoot),
      "Path of mcrouter fifo's directory.")
    ("filename-pattern,P",
      po::value<std::string>(&settings.filenamePattern),
      "Basic regular expression (BRE) to match the name of the fifos.")
    ("quiet,q",
      po::bool_switch(&settings.quiet)->default_value(false),
      "Doesn't display values.")
  ;

  // Positional arguments - hidden from the help message
  po::options_description hiddenOpts("Hidden options");
  hiddenOpts.add_options()
    ("match-expression",
     po::value<std::string>(&settings.matchExpression),
     "Match expression")
  ;
  po::positional_options_description posArgs;
  posArgs.add("match-expression", 1);

  // Parse command line
  po::variables_map vm;
  try {
    // Build all options
    po::options_description allOpts;
    allOpts.add(namedOpts).add(hiddenOpts);

    // Parse
    po::store(po::command_line_parser(argc, argv)
        .options(allOpts).positional(posArgs).run(), vm);
    po::notify(vm);
  } catch (po::error& ex) {
    LOG(ERROR) << ex.what();
    exit(1);
  }

  // Handles help
  if (vm.count("help")) {
    std::cout << getUsage(argv[0]);
    std::cout << std::endl;

    // Print only named options
    namedOpts.print(std::cout);
    exit(0);
  }

  // Check mandatory args
  CHECK(!settings.fifoRoot.empty())
    << "Fifo's directory (--fifo-root) cannot be empty";

  return settings;
}

/**
 * Matches all the occurences of "pattern" in "text"
 *
 * @return A vector of pairs containing the index and size (respectively)
 *         of all ocurrences.
 */
std::vector<std::pair<size_t, size_t>> matchAll(folly::StringPiece text,
                                                const boost::regex& pattern) {
  std::vector<std::pair<size_t, size_t>> result;

  boost::sregex_token_iterator it(text.begin(), text.end(), pattern);
  boost::sregex_token_iterator end;
  while (it != end) {
    result.emplace_back(it->first - text.begin(), it->length());
    ++it;
  }
  return result;
}

std::string serializeMessageHeader(const McMsgRef& msg) {
  std::string out;

  if (msg->op != mc_op_unknown) {
    out.append(mc_op_to_string(msg->op));
  }
  if (msg->result != mc_res_unknown) {
    if (out.size() > 0) {
      out.push_back(' ');
    }
    out.append(mc_res_to_string(msg->result));
  }
  if (msg->key.len) {
    if (out.size() > 0) {
      out.push_back(' ');
    }
    out.append(folly::backslashify(to<std::string>(msg->key)));
  }

  return out;
}

void msgReady(uint64_t reqid, McMsgRef msg) {
  if (msg->op == mc_op_end) {
    return;
  }

  StyledString out;

  out.append("{\n", kFormat.dataOpColor);

  // Msg header
  auto msgHeader = serializeMessageHeader(msg);
  if (!msgHeader.empty()) {
    out.append("  ");
    out.append(std::move(msgHeader), kFormat.headerColor);
  }

  // Msg attributes
  out.append("\n  reqid: ", kFormat.msgAttrColor);
  out.append(folly::sformat("0x{:x}", reqid), kFormat.dataValueColor);
  out.append("\n  flags: ", kFormat.msgAttrColor);
  out.append(folly::sformat("0x{:x}", msg->flags), kFormat.dataValueColor);
  if (msg->flags) {
    auto flagDesc = describeFlags(msg->flags);

    if (!flagDesc.empty()) {
      out.pushAppendColor(kFormat.attrColor);
      out.append(" [");
      bool first = true;
      for (auto& s : flagDesc) {
        if (!first) {
          out.append(", ");
        }
        first = false;
        out.append(s);
      }
      out.pushBack(']');
      out.popAppendColor();
    }
  }
  if (msg->exptime) {
      out.append("\n  exptime: ", kFormat.msgAttrColor);
      out.append(folly::format("{:d}", msg->exptime).str(),
                 kFormat.dataValueColor);
  }

  out.pushBack('\n');

  if (msg->value.len) {
    auto value = to<folly::StringPiece>(msg->value);
    size_t uncompressedSize;
    auto formattedValue =
      gValueFormatter->uncompressAndFormat(value,
                                           msg->flags,
                                           kFormat,
                                           uncompressedSize);

    out.append("  value size: ", kFormat.msgAttrColor);
    if (uncompressedSize != value.size()) {
      out.append(
        folly::sformat("{} uncompressed, {} compressed, {:.2f}% savings",
                       uncompressedSize, value.size(),
                       100.0 - 100.0 * value.size()/uncompressedSize),
        kFormat.dataValueColor);
    } else {
      out.append(folly::to<std::string>(value.size()), kFormat.dataValueColor);
    }

    if (!gSettings.quiet) {
      out.append("\n  value: ", kFormat.msgAttrColor);
      out.append(formattedValue);
    }
    out.pushBack('\n');
  }

  out.append("}\n", kFormat.dataOpColor);

  if (gDataPattern) {
    auto matches = matchAll(out.text(), *gDataPattern);
    if (matches.empty()) {
      return;
    }
    for (auto& m : matches) {
      out.setFg(m.first, m.second, kFormat.matchColor);
    }
  }

  gTargetOut << out;
  gTargetOut.flush();
}

/**
 * Builds the regex to match fifos' names.
 */
std::unique_ptr<boost::regex> buildFilenameRegex() noexcept {
  if (!gSettings.filenamePattern.empty()) {
    try {
      return folly::make_unique<boost::regex>(gSettings.filenamePattern,
                                              boost::regex_constants::basic);
    } catch (const std::exception& e) {
      LOG(ERROR) << "Invalid filename pattern: " << e.what();
      exit(1);
    }
  }
  return nullptr;
}

/**
 * Builds the regex to match data.
 *
 * @returns   The regex or null (to match everything).
 */
std::unique_ptr<boost::regex> buildDataRegex() noexcept {
  if (!gSettings.matchExpression.empty()) {
    try {
      return folly::make_unique<boost::regex>(gSettings.matchExpression,
                                              boost::regex_constants::basic);
    } catch (const std::exception& e) {
      LOG(ERROR) << "Invalid pattern: " << e.what();
      exit(1);
    }
  }
  return nullptr;
}

void run() {
  // Builds filename pattern
  auto filenamePattern = buildFilenameRegex();
  if (filenamePattern) {
    std::cout << "Filename pattern: " << *filenamePattern << std::endl;
  }

  // Builds data pattern
  gDataPattern = buildDataRegex();
  if (gDataPattern) {
    std::cout << "Data pattern: " << *gDataPattern << std::endl;
  }

  folly::EventBase evb;
  ParserMap parserMap(msgReady);
  FifoReaderManager fifoManager(evb, parserMap,
                                gSettings.fifoRoot, std::move(filenamePattern));

  evb.loopForever();
}

} // anonymous namespace

int main(int argc, char **argv) {
  google::InitGoogleLogging(argv[0]);
  gSettings = parseOptions(argc, argv);

  run();

  return 0;
}