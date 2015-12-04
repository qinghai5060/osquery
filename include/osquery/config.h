/*
 *  Copyright (c) 2014, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 */

#pragma once

#include <list>
#include <map>
#include <memory>
#include <vector>

#include <boost/iterator/filter_iterator.hpp>
#include <boost/property_tree/ptree.hpp>

#include <osquery/core.h>
#include <osquery/flags.h>
#include <osquery/packs.h>
#include <osquery/registry.h>
#include <osquery/status.h>

namespace osquery {

class ConfigParserPlugin;

/**
 * The schedule is an iterable collection of Packs. When you iterate through
 * a schedule, you only get the packs that should be running on the host that
 * you're currently operating on.
 */
class Schedule {
 public:
  /// Under the hood, the schedule is just a list of the Pack objects
  typedef std::list<Pack> container;

  /**
   * @brief Create a schedule maintained by the configuration.
   *
   * This will check for previously executing queries. If any query was
   * executing it is considered in a 'dirty' state and should generate logs.
   * The schedule may also choose to blacklist this query.
   */
  Schedule();

  /**
   * @brief this class' iteration function
   *
   * Our step operation will be called on each element in packs_. It is
   * responsible for determining if that element should be returned as the
   * next iterator element or skipped.
   */
  struct Step {
    bool operator()(Pack& pack) { return pack.shouldPackExecute(); }
  };

  /// Boost gives us a nice template for maintaining the state of the iterator
  typedef boost::filter_iterator<Step, container::iterator> iterator;

  /// Add a pack to the schedule
  void add(const Pack& pack) {
    remove(pack.getName(), pack.getSource());
    packs_.push_back(pack);
  }

  /// Remove a pack, by name.
  void remove(const std::string& pack) { remove(pack, ""); }

  void remove(const std::string& pack, const std::string& source) {
    packs_.remove_if([pack, source](Pack& p) {
      return (p.getName() == pack) && (p.getSource() == source);
    });
  }

  iterator begin() { return iterator(packs_.begin(), packs_.end()); }
  iterator end() { return iterator(packs_.end(), packs_.end()); }

 private:
  /// Underlying storage for the packs
  container packs_;

  /**
   * @brief The schedule will check and record previously executing queries.
   *
   * If a query is found on initialization, the name will be recorded, it is
   * possible to skip previously failed queries.
   */
  std::string failed_query_;

  /**
   * @brief List of blacklisted queries.
   *
   * A list of queries that are blacklisted from executing due to prior
   * failures. If a query caused a worker to fail it will be recorded during
   * the next execution and saved to the blacklist.
   */
  std::map<std::string, size_t> blacklist_;

 private:
  friend class Config;
};

/**
 * @brief The programatic representation of osquery's configuration
 *
 * @code{.cpp}
 *   auto c = Config::getInstance();
 *   // use methods in osquery::Config on `c`
 * @endcode
 */
class Config {
 private:
  Config() : schedule_(Schedule()), valid_(false), start_time_(time(nullptr)){};

 public:
  /// Get a singleton instance of the Config class
  static Config& getInstance() {
    static Config cfg;
    return cfg;
  };

  /**
   * @brief Call the genConfig method of the config retriever plugin.
   *
   * This may perform a resource load such as TCP request or filesystem read.
   */
  Status load();

  /**
   * @brief Update the internal config data.
   *
   * @param config A map of domain or namespace to config data.
   * @return If the config changes were applied.
   */
  Status update(const std::map<std::string, std::string>& config);

  /**
   * @brief Drain the entire schedule
   *
   * This is called whenever the config is re-loaded
   */
  void clearSchedule();

  /**
   * @brief Drain the file data
   *
   * This is called whenever the config is re-loaded
   */
  void clearFiles();

  /**
   * @brief Expire the string cache of the hash
   *
   * This is called whenever the config is re-loaded
   */
  void clearHash();

  /**
   * @brief Record performance (monitoring) information about a scheduled query.
   *
   * The daemon and query scheduler will optionally record process metadata
   * before and after executing each query. This can be compared and reported
   * on an interval or within the osquery_schedule table.
   *
   * The config consumes and calculates the optional performance differentials.
   * It would also be possible to store this in the RocksDB backing store or
   * report directly to a LoggerPlugin sink. The Config is the most appropriate
   * as the metrics are transient to the process running the schedule and apply
   * to the updates/changes reflected in the schedule, from the config.
   *
   * @param name The unique name of the scheduled item
   * @param delay Number of seconds (wall time) taken by the query
   * @param size Number of characters generated by query
   * @param r0 the process row before the query
   * @param r1 the process row after the query
   */
  void recordQueryPerformance(const std::string& name,
                              size_t delay,
                              size_t size,
                              const Row& r0,
                              const Row& r1);

  /**
   * @brief Record a query 'initialization', meaning the query will run.
   *
   * Recording initializations if queries helps to identify when queries do not
   * complete. The Config::recordQueryPerformance method will clear a dirty
   * status set by this method. This status is saved in the backing database
   * store. On process start, or worker state, if any dirty bit is set then
   * it is assumed that the current start is a result of a previous abort.
   *
   * @param name THe unique name of the scheduled item
   */
  void recordQueryStart(const std::string& name);

  /**
   * @brief Calculate the hash of the osquery config
   *
   * @return The MD5 of the osquery config
   */
  Status getMD5(std::string& hash);

  /**
   * @brief Hash a source's config data
   *
   * @param source is the place where the config content came from
   * @param content is the content of the config data for a given source
   */
  void hashSource(const std::string& source, const std::string& content);

  /// Whether or not the last loaded config was valid.
  bool isValid();

  /// Get start time of config.
  size_t getStartTime() const { return start_time_; }

  /**
   * @brief Add a pack to the osquery schedule
   */
  void addPack(const std::string& name,
               const std::string& source,
               const boost::property_tree::ptree& tree);

  /**
   * @brief Remove a pack from the osquery schedule
   */
  void removePack(const std::string& pack);

  /**
   * @brief Iterate through all packs
   */
  void packs(std::function<void(Pack& pack)> predicate);

  /**
   * @brief Add a file
   *
   * @param category is the category which the file exists in
   * @param path is the file path to add
   */
  void addFile(const std::string& category, const std::string& path);

  /**
   * @brief Map a function across the set of scheduled queries
   *
   * @param predicate is a function which accepts two parameters, the name of
   * the query and the ScheduledQuery struct of the queries data. predicate
   * will be called on each currently scheduled query
   *
   * @code{.cpp}
   *   std::map<std::string, ScheduledQuery> queries;
   *   Config::getInstance().scheduledQueries(
   *      ([&queries](const std::string& name, const ScheduledQuery& query) {
   *        queries[name] = query;
   *      }));
   * @endcode
   */
  void scheduledQueries(std::function<
      void(const std::string& name, const ScheduledQuery& query)> predicate);

  /**
   * @brief Map a function across the set of configured files
   *
   * @param predicate is a function which accepts two parameters, the name of
   * the file category and a vector of files in that category. predicate will be
   * called on each pair in files_
   *
   * @code{.cpp}
   *   std::map<std::string, std::vector<std::string>> file_map;
   *   Config::getInstance().files(
   *      ([&file_map](const std::string& category,
   *                   const std::vector<std::string>& files) {
   *        file_map[category] = files;
   *      }));
   * @endcode
   */
  void files(
      std::function<void(const std::string& category,
                         const std::vector<std::string>& files)> predicate);

  /**
   * @brief Get the performance stats for a specific query, by name
   *
   * @param name is the name of the query which you'd like to retrieve
   * @param predicate is a function which accepts a const reference to a
   * QueryPerformance struct. predicate will be called on name's related
   * QueryPerformance struct, if it exists.
   *
   * @code{.cpp}
   *   Config::getInstance().getPerformanceStats(
   *     "my_awesome_query",
   *     [](const QueryPerformance& query) {
   *       // use "query" here
   *     });
   * @endcode
   */
  void getPerformanceStats(
      const std::string& name,
      std::function<void(const QueryPerformance& query)> predicate);

  /**
   * @brief Helper to access config parsers via the registry
   *
   * This may return a nullptr if an exception is thrown attempting to retrieve
   * the requested config parser.
   *
   * @param parser is the string name of the parser that you want
   *
   * @return a shared pointer to the config parser plugin
   */
  static const std::shared_ptr<ConfigParserPlugin> getParser(
      const std::string& parser);

 protected:
  /// A step method for Config::update.
  Status updateSource(const std::string& name, const std::string& json);

 protected:
  Schedule schedule_;

  /// A set of performance stats for each query in the schedule.
  std::map<std::string, QueryPerformance> performance_;
  /// A set of named categories filled with filesystem globbing paths.
  std::map<std::string, std::vector<std::string> > files_;
  /// A set of hashes for each source of the config.
  std::map<std::string, std::string> hash_;
  bool valid_{false};
  /// A UNIX timestamp recorded when the config started.
  size_t start_time_{0};

 private:
  FRIEND_TEST(ConfigTests, test_parse);
  FRIEND_TEST(ConfigTests, test_remove);
  FRIEND_TEST(ConfigTests, test_get_scheduled_queries);
  FRIEND_TEST(ConfigTests, test_get_parser);
  FRIEND_TEST(ConfigTests, test_add_remove_pack);
  FRIEND_TEST(ConfigTests, test_noninline_pack);
  FRIEND_TEST(OptionsConfigParserPluginTests, test_get_option);
  FRIEND_TEST(FilePathsConfigParserPluginTests, test_get_files);
  FRIEND_TEST(PacksTests, test_discovery_cache);
};

/**
 * @brief Superclass for the pluggable config component.
 *
 * In order to make the distribution of configurations to hosts running
 * osquery, we take advantage of a plugin interface which allows you to
 * integrate osquery with your internal configuration distribution mechanisms.
 * You may use ZooKeeper, files on disk, a custom solution, etc. In order to
 * use your specific configuration distribution system, one simply needs to
 * create a custom subclass of ConfigPlugin. That subclass should implement
 * the ConfigPlugin::genConfig method.
 *
 * Consider the following example:
 *
 * @code{.cpp}
 *   class TestConfigPlugin : public ConfigPlugin {
 *    public:
 *     virtual Status genConfig(std::map<std::string, std::string>& config) {
 *       config["my_source"] = "{}";
 *       return Status(0, "OK");
 *     }
 *   };
 *
 *   REGISTER(TestConfigPlugin, "config", "test");
 *  @endcode
 */
class ConfigPlugin : public Plugin {
 public:
  /**
   * @brief Virtual method which should implemented custom config retrieval
   *
   * ConfigPlugin::genConfig should be implemented by a subclasses of
   * ConfigPlugin which needs to retrieve config data in a custom way.
   *
   * @param config The output ConfigSourceMap, a map of JSON to source names.
   *
   * @return A failure status will prevent the source map from merging.
   */
  virtual Status genConfig(std::map<std::string, std::string>& config) = 0;

  /**
   * @brief Virtual method which could implement custom query pack retrieval
   *
   * The default config syntax for query packs is like the following:
   *
   * @code
   *   {
   *     "packs": {
   *       "foo": {
   *         "version": "1.5.0",
   *         "platform:" "any",
   *         "queries": {
   *           // ...
   *         }
   *       }
   *     }
   *   }
   * @endcode
   *
   * Alternatively, you can define packs like the following as well:
   *
   * @code
   *   {
   *     "packs": {
   *       "foo": "/var/osquery/packs/foo.json",
   *       "bar": "/var/osquery/packs/bar.json"
   *     }
   *   }
   * @endcode
   *
   * If you defined the "value" of your pack as a string instead of an inline
   * data structure, then osquery will pass the responsibility of retrieving
   * the pack to the active config plugin. In the above example, it seems
   * obvious that the value is a local file path. Alternatively, if the
   * filesystem config plugin wasn't being used, the string could be a remote
   * URL, etc.
   *
   * genPack is not a pure virtual, so you don't have to define it if you don't
   * want to use the shortened query pack syntax. The default implementation
   * returns a failed status.
   *
   * @param name is the name of the query pack
   * @param value is the string based value that was provided with the pack
   * @param pack should be populated with the string JSON pack content
   *
   * @return a Status instance indicating the success or failure of the call
   */
  virtual Status genPack(const std::string& name,
                         const std::string& value,
                         std::string& pack);

  /// Main entrypoint for config plugin requests
  Status call(const PluginRequest& request, PluginResponse& response);
};

/**
 * @brief A pluggable configuration parser.
 *
 * An osquery config instance is populated from JSON using a ConfigPlugin.
 * That plugin may update the config data asynchronously and read from
 * several sources, as is the case with "filesystem" and reading multiple files.
 *
 * A ConfigParserPlugin will receive the merged configuration at osquery start
 * and the updated (still merged) config if any ConfigPlugin updates the
 * instance asynchronously. Each parser specifies a set of top-level JSON
 * keys to receive. The config instance will auto-merge the key values
 * from multiple sources if they are dictionaries or lists.
 *
 * If a top-level key is a dictionary, each source with the top-level key
 * will have its own dictionary keys merged and replaced based on the lexical
 * order of sources. For the "filesystem" config plugin this is the lexical
 * sorting of filenames. If the top-level key is a list, each source with the
 * top-level key will have its contents appended.
 *
 * Each config parser plugin will live alongside the config instance for the
 * life of the osquery process. The parser may perform actions at config load
 * and config update "time" as well as keep its own data members and be
 * accessible through the Config class API.
 */
class ConfigParserPlugin : public Plugin {
 public:
  /**
   * @brief Return a list of top-level config keys to receive in updates.
   *
   * The ::update method will receive a map of these keys with a JSON-parsed
   * property tree of configuration data.
   *
   * @return A list of string top-level JSON keys.
   */
  virtual std::vector<std::string> keys() = 0;

  /**
   * @brief Receive a merged property tree for each top-level config key.
   *
   * Called when the Config instance is initially loaded with data from the
   * active config plugin and when it is updated via an async ConfigPlugin
   * update. Every config parser will receive a map of merged data for each key
   * they requested in keys().
   *
   * @param config A JSON-parsed property tree map.
   * @return Failure if the parser should no longer receive updates.
   */
  virtual Status update(
      const std::map<std::string, boost::property_tree::ptree>& config) = 0;

  Status setUp();

  const boost::property_tree::ptree& getData() const { return data_; }

 protected:
  /// Allow the config parser to keep some global state.
  boost::property_tree::ptree data_;
};

/**
 * @brief Config plugin registry.
 *
 * This creates an osquery registry for "config" which may implement
 * ConfigPlugin. A ConfigPlugin's call API should make use of a genConfig
 * after reading JSON data in the plugin implementation.
 */
CREATE_REGISTRY(ConfigPlugin, "config");

/**
 * @brief ConfigParser plugin registry.
 *
 * This creates an osquery registry for "config_parser" which may implement
 * ConfigParserPlugin. A ConfigParserPlugin should not export any call actions
 * but rather have a simple property tree-accessor API through Config.
 */
CREATE_LAZY_REGISTRY(ConfigParserPlugin, "config_parser");
}
