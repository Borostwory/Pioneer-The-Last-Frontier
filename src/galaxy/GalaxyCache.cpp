// Copyright © 2008-2026 Pioneer Developers. See AUTHORS.txt for details
// Licensed under the terms of the GPL v3. See licenses/GPL-3.txt

#include "galaxy/GalaxyCache.h"

#include "Game.h"
#include "Pi.h"
#include "galaxy/Galaxy.h"
#include "galaxy/GalaxyGenerator.h"
#include "galaxy/Sector.h"
#include "galaxy/StarSystem.h"
#include "core/Log.h"
#include "profiler/Profiler.h"
#include <algorithm>
#include <utility>

//#define DEBUG_CACHE

//virtual

template <typename T, typename CompareT>
GalaxyObjectCache<T, CompareT>::~GalaxyObjectCache()
{
	for (Slave *s : m_slaves)
		s->MasterDeleted();
	assert(m_attic.empty()); // otherwise the objects will deregister at a cache that no longer exists
}

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::AddToCache(std::vector<RefCountedPtr<T>> &objects)
{
	PROFILE_SCOPED()
	for (auto it = objects.begin(), itEnd = objects.end(); it != itEnd; ++it) {
		auto inserted = m_attic.insert(std::make_pair(it->Get()->GetPath(), it->Get()));
		if (!inserted.second) {
			it->Reset(inserted.first->second);
		} else {
			(*it)->SetCache(this);
		}
	}
}

template <typename T, typename CompareT>
RefCountedPtr<T> GalaxyObjectCache<T, CompareT>::GetIfCached(const SystemPath &path) const
{
	RefCountedPtr<T> s;
	typename AtticMap::const_iterator i = m_attic.find(path);
	if (i != m_attic.end()) {
		s.Reset(i->second);
	}

	return s;
}

template <typename T, typename CompareT>
RefCountedPtr<T> GalaxyObjectCache<T, CompareT>::GetCached(const SystemPath &path)
{
	RefCountedPtr<T> s = this->GetIfCached(path);
	if (!s) {
		++m_cacheMisses;
		s = m_galaxy->GetGenerator()->Generate<T, Self>(RefCountedPtr<Galaxy>(m_galaxy), path, this);
		m_attic.insert(std::make_pair(path, s.Get()));
	} else {
		++m_cacheHits;
	}
	return s;
}

template <typename T, typename CompareT>
bool GalaxyObjectCache<T, CompareT>::HasCached(const SystemPath &path) const
{
	PROFILE_SCOPED()

	return (m_attic.find(path) != m_attic.end());
}

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::RemoveFromAttic(const SystemPath &path)
{
	m_attic.erase(path);
}

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::ClearCache()
{
	for (auto it = m_slaves.begin(), itEnd = m_slaves.end(); it != itEnd; ++it)
		(*it)->ClearCache();
}

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::OutputCacheStatistics(bool reset)
{
	Output("%s: misses: %llu, slave hits: %llu, master hits: %llu\n", CACHE_NAME.c_str(), m_cacheMisses, m_cacheHitsSlave, m_cacheHits);
	if (reset)
		m_cacheMisses = m_cacheHitsSlave = m_cacheHits = 0;
}

template <typename T, typename CompareT>
RefCountedPtr<typename GalaxyObjectCache<T, CompareT>::Slave> GalaxyObjectCache<T, CompareT>::NewSlaveCache()
{
	return RefCountedPtr<Slave>(new Slave(this, RefCountedPtr<Galaxy>(m_galaxy), Pi::GetAsyncJobQueue()));
}

template <typename T, typename CompareT>
GalaxyObjectCache<T, CompareT>::Slave::Slave(GalaxyObjectCache<T, CompareT> *master, RefCountedPtr<Galaxy> galaxy, JobQueue *jobQueue) :
	m_master(master),
	m_galaxy(galaxy),
	m_jobs(Pi::GetAsyncJobQueue())
{
	m_master->m_slaves.insert(this);
}

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::Slave::MasterDeleted()
{
	m_master = nullptr;
}

template <typename T, typename CompareT>
RefCountedPtr<T> GalaxyObjectCache<T, CompareT>::Slave::GetIfCached(const SystemPath &path)
{
	typename CacheMap::iterator i = m_cache.find(path);
	if (i != m_cache.end())
		return (*i).second;
	return RefCountedPtr<T>();
}

template <typename T, typename CompareT>
RefCountedPtr<T> GalaxyObjectCache<T, CompareT>::Slave::GetCached(const SystemPath &path)
{
	PROFILE_SCOPED()

	typename CacheMap::iterator i = m_cache.find(path);
	if (i != m_cache.end()) {
		if (m_master)
			++m_master->m_cacheHitsSlave;
		return (*i).second;
	}

	if (m_master) {
		auto inserted = m_cache.insert(std::make_pair(path, m_master->GetCached(path)));
		return inserted.first->second;
	} else {
		return RefCountedPtr<T>();
	}
}

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::Slave::Erase(const SystemPath &path) { m_cache.erase(path); }

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::Slave::Erase(const typename CacheMap::const_iterator &it) { m_cache.erase(it); }

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::Slave::ClearCache() { m_cache.clear(); }

template <typename T, typename CompareT>
GalaxyObjectCache<T, CompareT>::Slave::~Slave()
{
#ifdef DEBUG_CACHE
	unsigned unique = 0;
	for (auto it = m_cache.begin(); it != m_cache.end(); ++it)
		if (it->second->GetRefCount() == 1)
			unique++;
	Output("%s: Discarding slave cache with " SIZET_FMT " entries (%u to be removed)\n", CACHE_NAME.c_str(), m_cache.size(), unique);
#endif
	if (m_master)
		m_master->m_slaves.erase(this);
}

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::Slave::AddToCache(std::vector<RefCountedPtr<T>> &objects)
{
	if (m_master) {
		m_master->AddToCache(objects); // This modifies the vector to the sectors already in the master cache
		for (auto it = objects.begin(), itEnd = objects.end(); it != itEnd; ++it) {
			m_cache.insert(std::make_pair(it->Get()->GetPath(), *it));
		}
	}
}

template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::Slave::FillCache(const PathVector &paths, CacheFilledCallback callback)
{
	// allocate some space for what we're about to chunk up
	// there will be at most N paths, but potentially M<=N of those paths are already cached in another slave
	// so don't allocate storage upfront for the entire list of paths
	std::vector<std::unique_ptr<PathVector>> vec_paths;
	vec_paths.reserve(paths.size() / CACHE_JOB_SIZE + 1);
	vec_paths.emplace_back(new PathVector());

	// (filled in batches, it's one list, so that only one job is waiting at a time: see CacheJob::OnFinish)
	const size_t jobSize = m_batchSize > 0 ? paths.size() + 1 : CACHE_JOB_SIZE;

	PathVector *current_paths = vec_paths.back().get();
	current_paths->reserve(std::min<size_t>(jobSize, paths.size()));
#ifdef DEBUG_CACHE
	size_t alreadyCached = m_cache.size();
	unsigned masterCached = 0;
	unsigned toBeCreated = 0;
#endif

	// chop the paths into groups of jobSize
	for (const SystemPath &path : paths) {
		RefCountedPtr<T> s = m_master->GetIfCached(path);
		if (s) {
			m_cache[path] = s;
#ifdef DEBUG_CACHE
			++masterCached;
#endif
		} else {
			current_paths->push_back(path);

			if (current_paths->size() >= jobSize) {
				current_paths = vec_paths.emplace_back(new PathVector()).get();
				current_paths->reserve(jobSize);
			}
#ifdef DEBUG_CACHE
			++toBeCreated;
#endif
		}
	}

#ifdef DEBUG_CACHE
	Output("%s: FillCache: " SIZET_FMT " cached, %u in master cache, %u to be created, will use " SIZET_FMT " jobs\n", CACHE_NAME.c_str(),
		alreadyCached, masterCached, toBeCreated, vec_paths.size());
#endif

	// No paths need to be generated
	if (vec_paths.front()->empty()) {
		if (callback)
			callback();
	} else {
		// now add the batched jobs
		for (std::unique_ptr<PathVector> &vec : vec_paths)
			m_jobs.Order(new CacheJob(std::move(vec), this, m_galaxy, callback));
	}
}

template <typename T, typename CompareT>
GalaxyObjectCache<T, CompareT>::CacheJob::CacheJob(std::unique_ptr<std::vector<SystemPath>> path,
	Slave *slaveCache, RefCountedPtr<Galaxy> galaxy, CacheFilledCallback callback, size_t first) :
	Job(),
	m_paths(std::move(path)),
	m_slaveCache(slaveCache),
	m_galaxy(galaxy),
	m_galaxyGenerator(galaxy->GetGenerator()),
	m_callback(callback),
	m_first(first),
	m_end(m_paths->size())
{
	if (slaveCache->m_batchSize > 0)
		m_end = std::min(m_end, m_first + slaveCache->m_batchSize);
	m_objects.reserve(m_end - m_first);
}

//virtual
template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::CacheJob::OnRun() // RUNS IN ANOTHER THREAD!! MUST BE THREAD SAFE!
{
	PROFILE_SCOPED()
	for (size_t i = m_first; i < m_end; ++i)
		m_objects.push_back(m_galaxyGenerator->Generate<T, Self>(m_galaxy, (*m_paths)[i], nullptr));
}

//virtual
template <typename T, typename CompareT>
void GalaxyObjectCache<T, CompareT>::CacheJob::OnFinish() // runs in primary thread of the context
{
	PROFILE_SCOPED()
	m_slaveCache->AddToCache(m_objects);
	// The rest goes to the back of the queue, so other jobs on it don't wait for the whole list.
	if (m_end < m_paths->size())
		m_slaveCache->m_jobs.Order(new CacheJob(std::move(m_paths), m_slaveCache, m_galaxy, m_callback, m_end));
	else if (m_slaveCache->m_jobs.IsEmpty() && m_callback)
		m_callback();
}

/****** SectorCache ******/

template <>
const std::string SectorCache::CACHE_NAME("SectorCache");

// Explicit instantiation of SectorCache template
template class GalaxyObjectCache<Sector, SystemPath::LessSectorOnly>;

/****** StarSystemCache ******/

// Star systems are made on the main thread, as making one calls Lua, one job a frame and about 0.3 ms each: a few at a time,
// or arriving somewhere is slow for a while. It's only a prefetch: a system that's asked for before its turn is made then.
static constexpr size_t STAR_SYSTEMS_PER_JOB = 4;

template <>
StarSystemCache::Slave::Slave(Self *master, RefCountedPtr<Galaxy> galaxy, JobQueue *jobQueue) :
	m_master(master),
	m_galaxy(galaxy),
	m_jobs(Pi::GetSyncJobQueue()),
	m_batchSize(STAR_SYSTEMS_PER_JOB)
{
	m_master->m_slaves.insert(this);
}

template <>
const std::string StarSystemCache::CACHE_NAME("StarSystemCache");

// Explicit instantiation of StarSystemCache template
template class GalaxyObjectCache<StarSystem, SystemPath::LessSystemOnly>;
