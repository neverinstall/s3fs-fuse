/*
 * s3fs - FUSE-based file system backed by Amazon S3
 *
 * Copyright(C) 2007 Randy Rizun <rrizun@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <sys/stat.h>
#include <vector>

#include "s3fs.h"
#include "s3fs_logger.h"
#include "s3fs_util.h"
#include "cache.h"
#include "string_util.h"

//-------------------------------------------------------------------
// Utility
//-------------------------------------------------------------------
static void SetStatCacheTime(struct timespec& ts)
{
    if(-1 == clock_gettime(static_cast<clockid_t>(CLOCK_MONOTONIC_COARSE), &ts)){
        S3FS_PRN_CRIT("clock_gettime failed: %d", errno);
        abort();
    }
}

static int CompareStatCacheTime(const struct timespec& ts1, const struct timespec& ts2)
{
    // return -1:  ts1 < ts2
    //         0:  ts1 == ts2
    //         1:  ts1 > ts2
    if(ts1.tv_sec < ts2.tv_sec){
        return -1;
    }else if(ts1.tv_sec > ts2.tv_sec){
        return 1;
    }else{
        if(ts1.tv_nsec < ts2.tv_nsec){
            return -1;
        }else if(ts1.tv_nsec > ts2.tv_nsec){
            return 1;
        }
    }
    return 0;
}

static bool IsExpireStatCacheTime(const struct timespec& ts, time_t expire)
{
    struct timespec nowts;
    SetStatCacheTime(nowts);
    nowts.tv_sec -= expire;

    return (0 < CompareStatCacheTime(nowts, ts));
}

//
// For stats cache out 
//
typedef std::vector<stat_cache_t::iterator>   statiterlist_t;

struct sort_statiterlist{
    // ascending order
    bool operator()(const stat_cache_t::iterator& src1, const stat_cache_t::iterator& src2) const
    {
        int result = CompareStatCacheTime(src1->second.cache_date, src2->second.cache_date);
        if(0 == result){
            if(src1->second.hit_count < src2->second.hit_count){
                result = -1;
            }
        }
        return (result < 0);
    }
};

//
// For symbolic link cache out 
//
typedef std::vector<symlink_cache_t::iterator>   symlinkiterlist_t;

struct sort_symlinkiterlist{
    // ascending order
    bool operator()(const symlink_cache_t::iterator& src1, const symlink_cache_t::iterator& src2) const
    {
        int result = CompareStatCacheTime(src1->second.cache_date, src2->second.cache_date);  // use the same as Stats
        if(0 == result){
            if(src1->second.hit_count < src2->second.hit_count){
                result = -1;
            }
        }
        return (result < 0);
    }
};

//-------------------------------------------------------------------
// Static
//-------------------------------------------------------------------
StatCache       StatCache::singleton;
std::mutex      StatCache::stat_cache_lock;

//-------------------------------------------------------------------
// Constructor/Destructor
//-------------------------------------------------------------------
StatCache::StatCache() : IsExpireTime(true), IsExpireIntervalType(false), ExpireTime(15 * 60), CacheSize(100000), IsCacheNoObject(true)
{
    if(this == StatCache::getStatCacheData()){
        stat_cache.clear();
    }else{
        abort();
    }
}

StatCache::~StatCache()
{
    if(this == StatCache::getStatCacheData()){
        Clear();
    }else{
        abort();
    }
}

//-------------------------------------------------------------------
// Methods
//-------------------------------------------------------------------
unsigned long StatCache::GetCacheSize() const
{
    return CacheSize;
}

unsigned long StatCache::SetCacheSize(unsigned long size)
{
    unsigned long old = CacheSize;
    CacheSize = size;
    return old;
}

time_t StatCache::GetExpireTime() const
{
    return (IsExpireTime ? ExpireTime : (-1));
}

time_t StatCache::SetExpireTime(time_t expire, bool is_interval)
{
    time_t old           = ExpireTime;
    ExpireTime           = expire;
    IsExpireTime         = true;
    IsExpireIntervalType = is_interval;
    return old;
}

time_t StatCache::UnsetExpireTime()
{
    time_t old           = IsExpireTime ? ExpireTime : (-1);
    ExpireTime           = 0;
    IsExpireTime         = false;
    IsExpireIntervalType = false;
    return old;
}

bool StatCache::SetCacheNoObject(bool flag)
{
    bool old = IsCacheNoObject;
    IsCacheNoObject = flag;
    return old;
}

void StatCache::Clear()
{
    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);

    stat_cache.clear();
    S3FS_MALLOCTRIM(0);
}

bool StatCache::GetStat(const std::string& key, struct stat* pst, headers_t* meta, bool overcheck, const char* petag, bool* pisforce)
{
    bool is_delete_cache = false;
    std::string strpath = key;

    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);

    auto iter = stat_cache.end();
    if(overcheck && '/' != *strpath.rbegin()){
        strpath += "/";
        iter = stat_cache.find(strpath);
    }
    if(iter == stat_cache.end()){
        strpath = key;
        iter = stat_cache.find(strpath);
    }

    if(iter != stat_cache.end()){
        stat_cache_entry* ent = &iter->second;
        if(0 < ent->notruncate || !IsExpireTime || !IsExpireStatCacheTime(ent->cache_date, ExpireTime)){
            if(ent->noobjcache){
                if(!IsCacheNoObject){
                    // need to delete this cache.
                    DelStatHasLock(strpath);
                }else{
                    // noobjcache = true means no object.
                }
                return false;
            }
            // hit without checking etag
            std::string stretag;
            if(petag){
                // find & check ETag
                for(auto hiter = ent->meta.cbegin(); hiter != ent->meta.cend(); ++hiter){
                    std::string tag = lower(hiter->first);
                    if(tag == "etag"){
                        stretag = hiter->second;
                        if('\0' != petag[0] && petag != stretag){
                            is_delete_cache = true;
                        }
                        break;
                    }
                }
            }
            if(is_delete_cache){
                // not hit by different ETag
                S3FS_PRN_DBG("stat cache not hit by ETag[path=%s][time=%lld.%09ld][hit count=%lu][ETag(%s)!=(%s)]",
                    strpath.c_str(), static_cast<long long>(ent->cache_date.tv_sec), ent->cache_date.tv_nsec, ent->hit_count, petag ? petag : "null", stretag.c_str());
            }else{
                // hit 
                S3FS_PRN_DBG("stat cache hit [path=%s][time=%lld.%09ld][hit count=%lu]",
                    strpath.c_str(), static_cast<long long>(ent->cache_date.tv_sec), ent->cache_date.tv_nsec, ent->hit_count);

                if(pst!= nullptr){
                    *pst= ent->stbuf;
                }
                if(meta != nullptr){
                    *meta = ent->meta;
                }
                if(pisforce != nullptr){
                    (*pisforce) = ent->isforce;
                }
                ent->hit_count++;
  
                if(IsExpireIntervalType){
                    SetStatCacheTime(ent->cache_date);
                }
                return true;
            }

        }else{
            // timeout
            is_delete_cache = true;
        }
    }

    if(is_delete_cache){
        DelStatHasLock(strpath);
    }
    return false;
}

bool StatCache::IsNoObjectCache(const std::string& key, bool overcheck)
{
    bool is_delete_cache = false;
    std::string strpath = key;

    if(!IsCacheNoObject){
        return false;
    }

    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);

    auto iter = stat_cache.end();
    if(overcheck && '/' != *strpath.rbegin()){
        strpath += "/";
        iter     = stat_cache.find(strpath);
    }
    if(iter == stat_cache.end()){
        strpath = key;
        iter    = stat_cache.find(strpath);
    }

    if(iter != stat_cache.end()) {
        const stat_cache_entry* ent = &iter->second;
        if(0 < ent->notruncate || !IsExpireTime || !IsExpireStatCacheTime(iter->second.cache_date, ExpireTime)){
            if(iter->second.noobjcache){
                // noobjcache = true means no object.
                SetStatCacheTime((*iter).second.cache_date);
                return true;
            }
        }else{
            // timeout
            is_delete_cache = true;
        }
    }

    if(is_delete_cache){
        DelStatHasLock(strpath);
    }
    return false;
}

bool StatCache::AddStat(const std::string& key, const headers_t& meta, bool forcedir, bool no_truncate)
{
    if(!no_truncate && CacheSize< 1){
        return true;
    }
    S3FS_PRN_INFO3("add stat cache entry[path=%s]", key.c_str());

    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);

    if(stat_cache.cend() != stat_cache.find(key)){
        // found cache
        DelStatHasLock(key);
    }else{
        // check: need to truncate cache
        if(stat_cache.size() > CacheSize){
            // cppcheck-suppress unmatchedSuppression
            // cppcheck-suppress knownConditionTrueFalse
            if(!TruncateCache()){
                return false;
            }
        }
    }

    // make new
    stat_cache_entry ent;
    if(!convert_header_to_stat(key.c_str(), meta, &ent.stbuf, forcedir)){
        return false;
    }
    ent.hit_count  = 0;
    ent.isforce    = forcedir;
    ent.noobjcache = false;
    ent.notruncate = (no_truncate ? 1L : 0L);
    ent.meta.clear();
    SetStatCacheTime(ent.cache_date);    // Set time.
    //copy only some keys
    for(auto iter = meta.cbegin(); iter != meta.cend(); ++iter){
        std::string tag   = lower(iter->first);
        std::string value = iter->second;
        if(tag == "content-type"){
            ent.meta[iter->first] = value;
        }else if(tag == "content-length"){
            ent.meta[iter->first] = value;
        }else if(tag == "etag"){
            ent.meta[iter->first] = value;
        }else if(tag == "last-modified"){
            ent.meta[iter->first] = value;
        }else if(is_prefix(tag.c_str(), "x-amz")){
            ent.meta[tag] = value;      // key is lower case for "x-amz"
        }
    }

    const auto& value = stat_cache[key] = std::move(ent);

    // check symbolic link cache
    if(!S_ISLNK(value.stbuf.st_mode)){
        if(symlink_cache.cend() != symlink_cache.find(key)){
            // if symbolic link cache has key, thus remove it.
            DelSymlinkHasLock(key);
        }
    }

    // If no_truncate flag is set, set file name to notruncate_file_cache
    //
    if(no_truncate){
        AddNotruncateCache(key);
    }

    return true;
}

// [NOTE]
// Updates only meta data if cached data exists.
// And when these are updated, it also updates the cache time.
//
// Since the file mode may change while the file is open, it is
// updated as well.
//
bool StatCache::UpdateMetaStats(const std::string& key, const headers_t& meta)
{
    if(CacheSize < 1){
        return true;
    }
    S3FS_PRN_INFO3("update stat cache entry[path=%s]", key.c_str());

    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);
    auto iter = stat_cache.find(key);
    if(stat_cache.cend() == iter){
        return true;
    }
    stat_cache_entry* ent = &iter->second;

    // update only meta keys
    for(auto metaiter = meta.cbegin(); metaiter != meta.cend(); ++metaiter){
        std::string tag   = lower(metaiter->first);
        std::string value = metaiter->second;
        if(tag == "content-type"){
            ent->meta[metaiter->first] = value;
        }else if(tag == "content-length"){
            ent->meta[metaiter->first] = value;
        }else if(tag == "etag"){
            ent->meta[metaiter->first] = value;
        }else if(tag == "last-modified"){
            ent->meta[metaiter->first] = value;
        }else if(is_prefix(tag.c_str(), "x-amz")){
            ent->meta[tag] = value;      // key is lower case for "x-amz"
        }
    }

    // Update time.
    SetStatCacheTime(ent->cache_date);

    // Update only mode
    ent->stbuf.st_mode = get_mode(meta, key);

    return true;
}

bool StatCache::AddNoObjectCache(const std::string& key)
{
    if(!IsCacheNoObject){
        return true;    // pretend successful
    }
    if(CacheSize < 1){
        return true;
    }
    S3FS_PRN_INFO3("add no object cache entry[path=%s]", key.c_str());

    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);

    if(stat_cache.cend() != stat_cache.find(key)){
		// found
        DelStatHasLock(key);
    }else{
        // check: need to truncate cache
        if(stat_cache.size() > CacheSize){
            // cppcheck-suppress unmatchedSuppression
            // cppcheck-suppress knownConditionTrueFalse
            if(!TruncateCache()){
                return false;
            }
        }
    }

    // make new
    stat_cache_entry ent{};
    ent.hit_count  = 0;
    ent.isforce    = false;
    ent.noobjcache = true;
    ent.notruncate = 0L;
    ent.meta.clear();
    SetStatCacheTime(ent.cache_date);    // Set time.

    stat_cache[key] = std::move(ent);

    // check symbolic link cache
    if(symlink_cache.cend() != symlink_cache.find(key)){
        // if symbolic link cache has key, thus remove it.
        DelSymlinkHasLock(key);
    }
    return true;
}

void StatCache::ChangeNoTruncateFlag(const std::string& key, bool no_truncate)
{
    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);
    auto iter = stat_cache.find(key);

    if(stat_cache.cend() != iter){
        stat_cache_entry* ent = &iter->second;
        if(no_truncate){
            if(0L == ent->notruncate){
                // need to add no truncate cache.
                AddNotruncateCache(key);
            }
            ++(ent->notruncate);
        }else{
            if(0L < ent->notruncate){
                --(ent->notruncate);
                if(0L == ent->notruncate){
                    // need to delete from no truncate cache.
                    DelNotruncateCache(key);
                }
            }
        }
    }
}

bool StatCache::TruncateCache()
{
    if(stat_cache.empty()){
        return true;
    }

    // 1) erase over expire time
    if(IsExpireTime){
        for(auto iter = stat_cache.cbegin(); iter != stat_cache.cend(); ){
            const stat_cache_entry* entry = &iter->second;
            if(0L == entry->notruncate && IsExpireStatCacheTime(entry->cache_date, ExpireTime)){
                iter = stat_cache.erase(iter);
            }else{
                ++iter;
            }
        }
    }

    // 2) check stat cache count
    if(stat_cache.size() < CacheSize){
        return true;
    }

    // 3) erase from the old cache in order
    size_t            erase_count= stat_cache.size() - CacheSize + 1;
    statiterlist_t    erase_iters;
    for(auto iter = stat_cache.begin(); iter != stat_cache.end() && 0 < erase_count; ++iter){
        // check no truncate
        const stat_cache_entry* ent = &iter->second;
        if(0L < ent->notruncate){
            // skip for no truncate entry and keep extra counts for this entity.
            if(0 < erase_count){
                --erase_count;     // decrement
            }
        }else{
            // iter is not have notruncate flag
            erase_iters.push_back(iter);
        }
        if(erase_count < erase_iters.size()){
            std::sort(erase_iters.begin(), erase_iters.end(), sort_statiterlist());
            while(erase_count < erase_iters.size()){
                erase_iters.pop_back();
            }
        }
    }
    for(auto iiter = erase_iters.cbegin(); iiter != erase_iters.cend(); ++iiter){
        auto siter = *iiter;

        S3FS_PRN_DBG("truncate stat cache[path=%s]", siter->first.c_str());
        stat_cache.erase(siter);
    }
    S3FS_MALLOCTRIM(0);

    return true;
}

bool StatCache::DelStatHasLock(const std::string& key)
{
    S3FS_PRN_INFO3("delete stat cache entry[path=%s]", key.c_str());

    stat_cache_t::iterator iter;
    if(stat_cache.cend() != (iter = stat_cache.find(key))){
        stat_cache.erase(iter);
        DelNotruncateCache(key);
    }
    if(!key.empty() && key != "/"){
        std::string strpath = key;
        if('/' == *strpath.rbegin()){
            // If there is "path" cache, delete it.
            strpath.erase(strpath.length() - 1);
        }else{
            // If there is "path/" cache, delete it.
            strpath += "/";
        }
        if(stat_cache.cend() != (iter = stat_cache.find(strpath))){
            stat_cache.erase(iter);
            DelNotruncateCache(strpath);
        }
    }
    S3FS_MALLOCTRIM(0);

    return true;
}

bool StatCache::GetSymlink(const std::string& key, std::string& value)
{
    bool is_delete_cache = false;
    const std::string& strpath = key;

    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);

    auto iter = symlink_cache.find(strpath);
    if(iter != symlink_cache.cend()){
        symlink_cache_entry* ent = &iter->second;
        if(!IsExpireTime || !IsExpireStatCacheTime(ent->cache_date, ExpireTime)){   // use the same as Stats
            // found
            S3FS_PRN_DBG("symbolic link cache hit [path=%s][time=%lld.%09ld][hit count=%lu]",
                strpath.c_str(), static_cast<long long>(ent->cache_date.tv_sec), ent->cache_date.tv_nsec, ent->hit_count);

            value = ent->link;

            ent->hit_count++;
            if(IsExpireIntervalType){
                SetStatCacheTime(ent->cache_date);
            }
            return true;
        }else{
            // timeout
            is_delete_cache = true;
        }
    }

    if(is_delete_cache){
        DelSymlinkHasLock(strpath);
    }
    return false;
}

bool StatCache::AddSymlink(const std::string& key, const std::string& value)
{
    if(CacheSize< 1){
        return true;
    }
    S3FS_PRN_INFO3("add symbolic link cache entry[path=%s, value=%s]", key.c_str(), value.c_str());

    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);

    if(symlink_cache.cend() != symlink_cache.find(key)){
    	// found
        DelSymlinkHasLock(key);
    }else{
        // check: need to truncate cache
        if(symlink_cache.size() > CacheSize){
            // cppcheck-suppress unmatchedSuppression
            // cppcheck-suppress knownConditionTrueFalse
            if(!TruncateSymlink()){
                return false;
            }
        }
    }

    // make new
    symlink_cache_entry ent;
    ent.link       = value;
    ent.hit_count  = 0;
    SetStatCacheTime(ent.cache_date);    // Set time(use the same as Stats).

    symlink_cache[key] = std::move(ent);

    return true;
}

bool StatCache::TruncateSymlink()
{
    if(symlink_cache.empty()){
        return true;
    }

    // 1) erase over expire time
    if(IsExpireTime){
        for(auto iter = symlink_cache.cbegin(); iter != symlink_cache.cend(); ){
            const symlink_cache_entry* entry = &iter->second;
            if(IsExpireStatCacheTime(entry->cache_date, ExpireTime)){  // use the same as Stats
                iter = symlink_cache.erase(iter);
            }else{
                ++iter;
            }
        }
    }

    // 2) check stat cache count
    if(symlink_cache.size() < CacheSize){
        return true;
    }

    // 3) erase from the old cache in order
    size_t            erase_count= symlink_cache.size() - CacheSize + 1;
    symlinkiterlist_t erase_iters;
    for(auto iter = symlink_cache.begin(); iter != symlink_cache.end(); ++iter){
        erase_iters.push_back(iter);
        sort(erase_iters.begin(), erase_iters.end(), sort_symlinkiterlist());
        if(erase_count < erase_iters.size()){
            erase_iters.pop_back();
        }
    }
    for(auto iiter = erase_iters.cbegin(); iiter != erase_iters.cend(); ++iiter){
        auto siter = *iiter;

        S3FS_PRN_DBG("truncate symbolic link  cache[path=%s]", siter->first.c_str());
        symlink_cache.erase(siter);
    }
    S3FS_MALLOCTRIM(0);

    return true;
}

bool StatCache::DelSymlinkHasLock(const std::string& key)
{
    S3FS_PRN_INFO3("delete symbolic link cache entry[path=%s]", key.c_str());

    symlink_cache_t::iterator iter;
    if(symlink_cache.cend() != (iter = symlink_cache.find(key))){
        symlink_cache.erase(iter);
    }
    S3FS_MALLOCTRIM(0);

    return true;
}

bool StatCache::AddNotruncateCache(const std::string& key)
{
    if(key.empty() || '/' == *key.rbegin()){
        return false;
    }

    std::string parentdir = mydirname(key);
    std::string filename  = mybasename(key);
    if(parentdir.empty() || filename.empty()){
        return false;
    }
    parentdir += '/';       // directory path must be '/' termination.

    auto iter = notruncate_file_cache.find(parentdir);
    if(iter == notruncate_file_cache.cend()){
        // add new list
        notruncate_filelist_t list;
        list.push_back(filename);
        notruncate_file_cache[parentdir] = list;
    }else{
        // add filename to existed list
        notruncate_filelist_t& filelist = iter->second;
        auto fiter = std::find(filelist.cbegin(), filelist.cend(), filename);
        if(fiter == filelist.cend()){
            filelist.push_back(filename);
        }
    }
    return true;
}

bool StatCache::DelNotruncateCache(const std::string& key)
{
    if(key.empty() || '/' == *key.rbegin()){
        return false;
    }

    std::string parentdir = mydirname(key);
    std::string filename  = mybasename(key);
    if(parentdir.empty() || filename.empty()){
        return false;
    }
    parentdir += '/';       // directory path must be '/' termination.

    auto iter = notruncate_file_cache.find(parentdir);
    if(iter != notruncate_file_cache.cend()){
        // found directory in map
        notruncate_filelist_t& filelist = iter->second;
        auto fiter = std::find(filelist.begin(), filelist.end(), filename);
        if(fiter != filelist.cend()){
            // found filename in directory file list
            filelist.erase(fiter);
            if(filelist.empty()){
                notruncate_file_cache.erase(parentdir);
            }
        }
    }
    return true;
}

// [Background]
// When s3fs creates a new file, the file does not exist until the file contents
// are uploaded.(because it doesn't create a 0 byte file)
// From the time this file is created(opened) until it is uploaded(flush), it
// will have a Stat cache with the No truncate flag added.
// This avoids file not existing errors in operations such as chmod and utimens
// that occur in the short period before file upload.
// Besides this, we also need to support readdir(list_bucket), this method is
// called to maintain the cache for readdir and return its value.
//
// [NOTE]
// Add the file names under parentdir to the list.
// However, if the same file name exists in the list, it will not be added.
// parentdir must be terminated with a '/'.
//
bool StatCache::GetNotruncateCache(const std::string& parentdir, notruncate_filelist_t& list)
{
    if(parentdir.empty()){
        return false;
    }

    std::string dirpath = parentdir;
    if('/' != *dirpath.rbegin()){
        dirpath += '/';
    }

    const std::lock_guard<std::mutex> lock(StatCache::stat_cache_lock);

    auto iter = notruncate_file_cache.find(dirpath);
    if(iter == notruncate_file_cache.cend()){
        // not found directory map
        return true;
    }

    // found directory in map
    const notruncate_filelist_t& filelist = iter->second;
    for(auto fiter = filelist.cbegin(); fiter != filelist.cend(); ++fiter){
        if(list.cend() == std::find(list.cbegin(), list.cend(), *fiter)){
           // found notuncate file that does not exist in the list, so add it.
           list.push_back(*fiter);
        }
    }
    return true;
}

//-------------------------------------------------------------------
// Functions
//-------------------------------------------------------------------
bool convert_header_to_stat(const char* path, const headers_t& meta, struct stat* pst, bool forcedir)
{
    if(!path || !pst){
        return false;
    }
    *pst = {};

    pst->st_nlink = 1; // see fuse FAQ

    // mode
    pst->st_mode = get_mode(meta, path, true, forcedir);

    // blocks
    if(S_ISREG(pst->st_mode)){
        pst->st_blocks = get_blocks(pst->st_size);
    }
    pst->st_blksize = 4096;

    // mtime
    struct timespec mtime = get_mtime(meta);
    if(pst->st_mtime < 0){
        pst->st_mtime = 0L;
    }else{
        if(mtime.tv_sec < 0){
            mtime.tv_sec  = 0;
            mtime.tv_nsec = 0;
        }
        set_timespec_to_stat(*pst, stat_time_type::MTIME, mtime);
    }

    // ctime
    struct timespec ctime = get_ctime(meta);
    if(pst->st_ctime < 0){
        pst->st_ctime = 0L;
    }else{
        if(ctime.tv_sec < 0){
            ctime.tv_sec  = 0;
            ctime.tv_nsec = 0;
        }
        set_timespec_to_stat(*pst, stat_time_type::CTIME, ctime);
    }

    // atime
    struct timespec atime = get_atime(meta);
    if(pst->st_atime < 0){
        pst->st_atime = 0L;
    }else{
        if(atime.tv_sec < 0){
            atime.tv_sec  = 0;
            atime.tv_nsec = 0;
        }
        set_timespec_to_stat(*pst, stat_time_type::ATIME, atime);
    }

    // size
    if(S_ISDIR(pst->st_mode)){
        pst->st_size = 4096;
    }else{
        pst->st_size = get_size(meta);
    }

    // uid/gid
    pst->st_uid = get_uid(meta);
    pst->st_gid = get_gid(meta);

    return true;
}

/*
* Local variables:
* tab-width: 4
* c-basic-offset: 4
* End:
* vim600: expandtab sw=4 ts=4 fdm=marker
* vim<600: expandtab sw=4 ts=4
*/
