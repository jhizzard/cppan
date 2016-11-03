/*
 * Copyright (c) 2016, Egor Pugin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     1. Redistributions of source code must retain the above copyright
 *        notice, this list of conditions and the following disclaimer.
 *     2. Redistributions in binary form must reproduce the above copyright
 *        notice, this list of conditions and the following disclaimer in the
 *        documentation and/or other materials provided with the distribution.
 *     3. Neither the name of the copyright holder nor the names of
 *        its contributors may be used to endorse or promote products
 *        derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "response.h"

#include "config.h"
#include "database.h"
#include "directories.h"
#include "exceptions.h"
#include "executor.h"
#include "lock.h"
#include "hasher.h"
#include "log.h"
#include "project.h"
#include "sqlite_database.h"

#include "logger.h"
DECLARE_STATIC_LOGGER(logger, "response");

#define CURRENT_API_LEVEL 1

TYPED_EXCEPTION(LocalDbHashException);

ResponseData rd;

void ResponseData::init(Config *config, const String &host)
{
    if (initialized)
        return;

    this->host = host;

    // remove from here?
    // add default (current, root) config
    packages[Package()].config = config;

    initialized = true;
}

void ResponseData::download_dependencies(const Packages &deps)
{
    if (executed || !initialized)
        return;

    if (deps.empty())
        return;

    // do 2 attempts: 1) local db, 2) remote db
    int n_attempts = 2;
    while (n_attempts--)
    {
        // clear before proceed
        download_dependencies_.clear();

        try
        {
            if (query_local_db)
            {
                try
                {
                    getDependenciesFromDb(deps);
                }
                catch (std::exception &e)
                {
                    LOG_ERROR(logger, "Cannot get dependencies from local database: " << e.what());

                    query_local_db = false;
                    getDependenciesFromRemote(deps);
                }
            }
            else
            {
                getDependenciesFromRemote(deps);
            }

            download_and_unpack();
        }
        catch (LocalDbHashException &)
        {
            LOG_WARN(logger, "Local db data caused issues, trying remote one");

            query_local_db = false;
            continue;
        }
        break;
    }

    read_configs();
    post_download();
    write_index();
    check_deps_changed();

    // add default (current, root) config
    packages[Package()].dependencies = deps;
    for (auto &dd : download_dependencies_)
    {
        if (!dd.second.flags[pfDirectDependency])
            continue;
        auto &deps2 = packages[Package()].dependencies;
        auto i = deps2.find(dd.second.ppath.toString());
        if (i == deps2.end())
        {
            // check if we chosen a root project match all subprojects
            Packages to_add;
            std::set<String> to_remove;
            for (auto &root_dep : deps2)
            {
                for (auto &child_dep : download_dependencies_)
                {
                    if (root_dep.second.ppath.is_root_of(child_dep.second.ppath))
                    {
                        to_add.insert({ child_dep.second.ppath.toString(), child_dep.second });
                        to_remove.insert(root_dep.second.ppath.toString());
                    }
                }
            }
            if (to_add.empty())
                throw std::runtime_error("cannot match dependency");
            for (auto &r : to_remove)
                deps2.erase(r);
            for (auto &a : to_add)
                deps2.insert(a);
            continue;
        }
        auto &d = i->second;
        d.version = dd.second.version;
        d.flags |= dd.second.flags;
        d.createNames();
    }

    // last in function
    executed = true;
}

void ResponseData::check_deps_changed()
{
    // already executed
    if (deps_changed)
        return;

    // deps are now resolved
    // now refresh dependencies database only for remote packages
    // this file (local,current,root) packages will be refreshed anyway
    auto &sdb = getServiceDatabase();
    for (auto &cc : packages)
    {
        Hasher h;
        for (auto &d : cc.second.dependencies)
            h |= d.second.target_name;
        if (!sdb.hasPackageDependenciesHash(cc.first, h.hash))
        {
            deps_changed = true;

            // clear exports for this project, so it will be regenerated
            auto p = Printer::create(cc.second.config->settings.printerType);
            p->clear_export(cc.first.getDirObj());
            cleanPackages(cc.first.target_name, CleanTarget::Lib | CleanTarget::Bin);
        }
        sdb.setPackageDependenciesHash(cc.first, h.hash);
    }
}

void ResponseData::getDependenciesFromRemote(const Packages &deps)
{
    // prepare request
    for (auto &d : deps)
    {
        ptree version;
        version.put("version", d.second.version.toString());
        request.put_child(ptree::path_type(d.second.ppath.toString(), '|'), version);
    }

    LOG_NO_NEWLINE("Requesting dependency list... ");
    {
        int n_tries = 3;
        while (1)
        {
            try
            {
                HttpRequest req = httpSettings;
                req.type = HttpRequest::POST;
                req.url = host + "/api/find_dependencies";
                req.data = ptree2string(request);
                auto resp = url_request(req);
                if (resp.http_code != 200)
                    throw std::runtime_error("Cannot get deps");
                dependency_tree = string2ptree(resp.response);
                break;
            }
            catch (...)
            {
                if (--n_tries == 0)
                    throw;
                LOG_NO_NEWLINE("Retrying... ");
            }
        }
    }

    // read deps urls, download them, unpack
    int api = 0;
    if (dependency_tree.find("api") != dependency_tree.not_found())
        api = dependency_tree.get<int>("api");

    auto e = dependency_tree.find("error");
    if (e != dependency_tree.not_found())
        throw std::runtime_error(e->second.get_value<String>());

    auto info = dependency_tree.find("info");
    if (info != dependency_tree.not_found())
        std::cout << info->second.get_value<String>() << "\n";

    if (api == 0)
        throw std::runtime_error("API version is missing in the response");
    if (api > CURRENT_API_LEVEL)
        throw std::runtime_error("Server uses more new API version. Please, upgrade the cppan client from site or via --self-upgrade");
    if (api < CURRENT_API_LEVEL - 1)
        throw std::runtime_error("Your client's API is newer than server's. Please, wait for server upgrade");

    if (dependency_tree.find("data_dir") != dependency_tree.not_found())
        data_url = dependency_tree.get<String>("data_dir");

    // dependencies were received without error
    LOG("Ok");

    // set dependencies
    auto &remote_packages = dependency_tree.get_child("packages");
    for (auto &v : remote_packages)
    {
        auto id = v.second.get<ProjectVersionId>("id");

        DownloadDependency d;
        d.ppath = v.first;
        d.version = v.second.get<String>("version");
        d.flags = decltype(d.flags)(v.second.get<uint64_t>("flags"));
        d.sha256 = v.second.get<String>("sha256");
        d.createNames();
        dep_ids[d] = id;

        if (v.second.find(DEPENDENCIES_NODE) != v.second.not_found())
        {
            std::set<ProjectVersionId> idx;
            for (auto &tree_dep : v.second.get_child(DEPENDENCIES_NODE))
                idx.insert(tree_dep.second.get_value<ProjectVersionId>());
            d.setDependencyIds(idx);
        }

        d.map_ptr = &download_dependencies_;
        download_dependencies_[id] = d;
    }
}

void ResponseData::getDependenciesFromDb(const Packages &deps)
{
    auto &db = getPackagesDatabase();
    auto dl_deps = db.findDependencies(deps);

    // set dependencies
    for (auto &v : dl_deps)
    {
        auto &d = v.second;
        d.createNames();
        dep_ids[d] = d.id;
        d.map_ptr = &download_dependencies_;
        download_dependencies_[d.id] = d;
    }
}

void ResponseData::download_and_unpack()
{
    auto download_dependency = [this](auto &dd)
    {
        auto &d = dd.second;
        auto version_dir = d.getDirSrc();
        auto hash_file = d.getStampFilename();

        // store hash of archive
        bool must_download = false;
        {
            std::ifstream ifile(hash_file.string());
            String hash;
            if (ifile)
            {
                ifile >> hash;
                ifile.close();
            }
            if (hash != d.sha256 || d.sha256.empty() || hash.empty())
                must_download = true;
        }

        if (fs::exists(version_dir) && !must_download)
            return;

        // lock, so only one cppan process at the time could download the project
        ScopedFileLock lck(hash_file, std::defer_lock);
        if (!lck.try_lock())
        {
            // wait & continue
            ScopedFileLock lck2(hash_file);
            add_config(d);
            return;
        }

        // remove existing version dir
        cleanPackages(d.target_name);

        auto fs_path = ProjectPath(d.ppath).toFileSystemPath().string();
        std::replace(fs_path.begin(), fs_path.end(), '\\', '/');
        String cppan_package_url = host + "/" + data_url + "/" + fs_path + "/" + d.version.toString() + ".tar.gz";
        String github_package_url = "https://github.com/cppan-packages/" + d.getHash() + "/raw/master/" + make_archive_name();
        path fn = version_dir.string() + ".tar.gz";

        String dl_hash;
        DownloadData ddata;
        ddata.fn = fn;
        ddata.sha256.hash = &dl_hash;

        LOG_INFO(logger, "Downloading: " << d.target_name << "...");

        auto download_from_url = [this, &ddata, &dl_hash, &d](const auto &url, bool nothrow = true)
        {
            ddata.url = url;
            try
            {
                download_file(ddata);
            }
            catch (...)
            {
                if (nothrow)
                    return false;
                throw;
            }

            if (dl_hash != d.sha256)
            {
                if (nothrow)
                    return false;

                // if we get hashes from local db
                // they can be stalled within server refresh time (15 mins)
                // in this case we should do request to server
                if (query_local_db)
                    throw LocalDbHashException("Hashes do not match for package: " + d.target_name);
                throw std::runtime_error("Hashes do not match for package: " + d.target_name);
            }

            return true;
        };

        // at first we try to download from github
        // if we failed,try from cppan (this should be removed)
        if (!download_from_url(github_package_url, !query_local_db))
        {
            LOG_ERROR(logger, "Fallback to cppan.org");
            download_from_url(cppan_package_url, false);
        }

        downloads++;
        write_file(hash_file, d.sha256);

        LOG_INFO(logger, "Unpacking  : " << d.target_name << "...");
        Files files;
        try
        {
            files = unpack_file(fn, version_dir);
        }
        catch (...)
        {
            fs::remove_all(version_dir);
            throw;
        }
        fs::remove(fn);

        // re-read in any case
        // no need to remove old config, let it die with program
        auto c = add_config(d);

        // move all files under unpack dir
        auto ud = c->getDefaultProject().unpack_directory;
        if (!ud.empty())
        {
            ud = version_dir / ud;
            if (fs::exists(ud))
                throw std::runtime_error("Cannot create unpack_directory '" + ud.string() + "' because fs object with the same name alreasy exists");
            fs::create_directories(ud);
            for (auto &f : boost::make_iterator_range(fs::directory_iterator(version_dir), {}))
            {
                if (f == ud || f.path().filename() == CPPAN_FILENAME)
                    continue;
                if (fs::is_directory(f))
                {
                    copy_dir(f, ud / f.path().filename());
                    fs::remove_all(f);
                }
                else if (fs::is_regular_file(f))
                {
                    fs::copy_file(f, ud / f.path().filename());
                    fs::remove(f);
                }
            }
        }
    };

    Executor e(get_max_threads(8));
    e.throw_exceptions = true;

    // threaded execution does not preserve object creation/destruction order,
    // so current path is not correctly restored
    ScopedCurrentPath cp;

    for (auto &dd : download_dependencies_)
        e.push([&download_dependency, &dd] { download_dependency(dd); });

    e.wait();

    if (query_local_db)
    {
        // send download list
        // remove this when cppan will be widely used
        // also because this download count can be easily abused
        getExecutor().push([this]()
        {
            ptree request;
            ptree children;
            for (auto &d : download_dependencies_)
            {
                ptree c;
                c.put("", d.second.id);
                children.push_back(std::make_pair("", c));
            }
            request.add_child("vids", children);

            try
            {
                HttpRequest req = httpSettings;
                req.type = HttpRequest::POST;
                req.url = host + "/api/add_downloads";
                req.data = ptree2string(request);
                auto resp = url_request(req);
            }
            catch (...)
            {
            }
        });

        // send download action once
        static std::once_flag flag;
        std::call_once(flag, [this]
        {
            getExecutor().push([this]
            {
                try
                {
                    HttpRequest req = httpSettings;
                    req.type = HttpRequest::POST;
                    req.url = host + "/api/add_client_call";
                    req.data = "{}"; // empty json
                    auto resp = url_request(req);
                }
                catch (...)
                {
                }
            });
        });
    }
}

void ResponseData::post_download()
{
    for (auto &cc : packages)
        prepare_config(cc);
}

void ResponseData::prepare_config(PackageConfigs::value_type &cc)
{
    auto &p = cc.first;
    auto &c = cc.second.config;
    auto &dependencies = cc.second.dependencies;
    c->is_dependency = true;
    c->setPackage(p);
    auto &project = c->getDefaultProject();

    if (p.flags[pfLocalProject])
        return;

    // prepare deps: extract real deps flags from configs
    for (auto &dep : download_dependencies_[dep_ids[p]].getDirectDependencies())
    {
        auto d = dep.second;
        auto i = project.dependencies.find(d.ppath.toString());
        if (i == project.dependencies.end())
        {
            // check if we chose a root project that matches all subprojects
            Packages to_add;
            std::set<String> to_remove;
            for (auto &root_dep : project.dependencies)
            {
                for (auto &child_dep : download_dependencies_[dep_ids[p]].getDirectDependencies())
                {
                    if (root_dep.second.ppath.is_root_of(child_dep.second.ppath))
                    {
                        to_add.insert({ child_dep.second.ppath.toString(), child_dep.second });
                        to_remove.insert(root_dep.second.ppath.toString());
                    }
                }
            }
            if (to_add.empty())
                throw std::runtime_error("dependency '" + d.ppath.toString() + "' is not found");
            for (auto &r : to_remove)
                project.dependencies.erase(r);
            for (auto &a : to_add)
                project.dependencies.insert(a);
            continue;
        }
        d.flags[pfIncludeDirectoriesOnly] = i->second.flags[pfIncludeDirectoriesOnly];
        i->second.version = d.version;
        i->second.flags = d.flags;
        dependencies.emplace(d.ppath.toString(), d);
    }

    c->post_download();
}

ResponseData::PackageConfig &ResponseData::operator[](const Package &p)
{
    return packages[p];
}

const ResponseData::PackageConfig &ResponseData::operator[](const Package &p) const
{
    auto i = packages.find(p);
    if (i == packages.end())
        throw std::runtime_error("Package not found: " + p.getTargetName());
    return i->second;
}

ResponseData::iterator ResponseData::begin()
{
    auto i = packages.find(Package());
    if (i != packages.end())
        return ++i;
    return packages.begin();
}

ResponseData::iterator ResponseData::end()
{
    return packages.end();
}

ResponseData::const_iterator ResponseData::begin() const
{
    auto i = packages.find(Package());
    if (i != packages.end())
        return ++i;
    return packages.begin();
}

ResponseData::const_iterator ResponseData::end() const
{
    return packages.end();
}

void ResponseData::write_index() const
{
    auto &sdb = getServiceDatabase();
    for (auto &cc : *this)
        sdb.addInstalledPackage(cc.first);
}

void ResponseData::read_configs()
{
    LOG_NO_NEWLINE("Reading package specs... ");
    for (auto &d : download_dependencies_)
        read_config(d.second);
    LOG("Ok");
}

void ResponseData::read_config(const DownloadDependency &d)
{
    if (!fs::exists(d.getDirSrc()))
        return;

    try
    {
        auto p = config_store.insert(std::make_unique<Config>(d.getDirSrc()));
        packages[d].config = p.first->get();
    }
    catch (std::exception &)
    {
        // something wrong, remove the whole dir to re-download it
        fs::remove_all(d.getDirSrc());
    }
}

Executor &ResponseData::getExecutor()
{
    if (executor)
        return *executor;
    // create small amount of thread at the moment
    // because we use it very rarely and not extensively
    executor = std::make_unique<Executor>(2);
    return *executor;
}

Config *ResponseData::add_config(std::unique_ptr<Config> &&config, bool created)
{
    auto cfg = config.get();
    auto i = config_store.insert(std::move(config));
    packages[cfg->pkg].config = i.first->get();
    packages[cfg->pkg].config->created = created;
    return packages[cfg->pkg].config;
}

Config *ResponseData::add_config(const Package &p)
{
    auto c = std::make_unique<Config>(p.getDirSrc());
    c->setPackage(p);
    return add_config(std::move(c), true);
}

Config *ResponseData::add_local_config(const Config &co)
{
    auto cu = std::make_unique<Config>(co);
    auto cp = add_config(std::move(cu), true);
    packages[cp->pkg].dependencies = cp->getDefaultProject().dependencies;
    // batch resolve first?
    for (auto &p : cp->getDefaultProject().dependencies)
        add_config(p.second); // resolve first; create names if not created during resolving; merge flags
    return cp;
}
