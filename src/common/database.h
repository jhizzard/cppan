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

#pragma once

#include "cppan_string.h"
#include "date_time.h"
#include "dependency.h"
#include "filesystem.h"

#include <chrono>
#include <memory>
#include <vector>

class SqliteDatabase;
struct Package;

struct TableDescriptor
{
    String name;
    String query;
};

using TableDescriptors = const std::vector<TableDescriptor>;

struct StartupAction
{
    enum Type
    {
        // append only
        ClearCache = 0,
        ServiceDbClearConfigHashes,
        CheckSchema,
    };

    int id;
    int action;
};

class Database
{
public:
    Database(const String &name, const TableDescriptors &tds);
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;

protected:
    std::unique_ptr<SqliteDatabase> db;
    path fn;
    path db_dir;
    bool created = false;
    const TableDescriptors &tds;

    void open(bool read_only = false);
    void recreate();
};

class ServiceDatabase : public Database
{
public:
    ServiceDatabase();

    void performStartupActions() const;

    void checkForUpdates() const;
    TimePoint getLastClientUpdateCheck() const;
    void setLastClientUpdateCheck(const TimePoint &p = Clock::now()) const;

    int getNumberOfRuns() const;
    int increaseNumberOfRuns() const; // returns previous value

    int getPackagesDbSchemaVersion() const;
    void setPackagesDbSchemaVersion(int version) const;

    bool isActionPerformed(const StartupAction &action) const;
    void setActionPerformed(const StartupAction &action) const;

    String getConfigByHash(const String &settings_hash) const;
    void addConfigHash(const String &settings_hash, const String &config, const String &config_hash) const;
    void clearConfigHashes() const;

    void setPackageDependenciesHash(const Package &p, const String &hash) const;
    bool hasPackageDependenciesHash(const Package &p, const String &hash) const;

    void addInstalledPackage(const Package &p) const;
    void removeInstalledPackage(const Package &p) const;
    String getInstalledPackageHash(const Package &p) const;
    int getInstalledPackageId(const Package &p) const;
    std::set<Package> getInstalledPackages() const;

    void setSourceGroups(const Package &p, const SourceGroups &sg) const;
    SourceGroups getSourceGroups(const Package &p) const;
    void removeSourceGroups(const Package &p) const;
    void removeSourceGroups(int id) const;

    Stamps getFileStamps() const;
    void setFileStamps(const Stamps &stamps) const;
    void clearFileStamps() const;

private:
    void createTables() const;
    void checkStamp() const;

    String getTableHash(const String &table) const;
    void setTableHash(const String &table, const String &hash) const;

    void recreateTable(const TableDescriptor &td) const;
};

class PackagesDatabase : public Database
{
    using Dependencies = DownloadDependency::DbDependencies;
    using DependenciesMap = std::map<Package, DownloadDependency>;

public:
    PackagesDatabase();

    IdDependencies findDependencies(const Packages &deps) const;

    void listPackages(const String &name = String());

private:
    path db_repo_dir;

    void download();
    void load(bool drop = false);

    void writeDownloadTime() const;
    TimePoint readDownloadTime() const;

    bool isCurrentDbOld() const;

    ProjectVersionId getExactProjectVersionId(const DownloadDependency &project, Version &version, ProjectFlags &flags, String &sha256) const;
    Dependencies getProjectDependencies(ProjectVersionId project_version_id, DependenciesMap &dm) const;
};

ServiceDatabase &getServiceDatabase();
PackagesDatabase &getPackagesDatabase();

int readPackagesDbSchemaVersion(const path &dir);
void writePackagesDbSchemaVersion(const path &dir);

int readPackagesDbVersion(const path &dir);
void writePackagesDbVersion(const path &dir, int version);