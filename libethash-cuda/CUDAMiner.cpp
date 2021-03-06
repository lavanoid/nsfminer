
#include <libethcore/Farm.h>
#include <ethash/ethash.hpp>

#include "CUDAMiner.h"

using namespace std;
using namespace dev;
using namespace eth;

struct CUDAChannel : public LogChannel
{
    static bool name() { return false; }
    static const int verbosity = 2;
};
#define cudalog clog(CUDAChannel)

CUDAMiner::CUDAMiner(unsigned _index, DeviceDescriptor& _device) : Miner("cu-", _index)
{
    m_deviceDescriptor = _device;
    m_block_multiple = 1000;
}

CUDAMiner::~CUDAMiner()
{
    stopWorking();
    kick_miner();
}

#define HostToDevice(dst, src, siz) CUDA_CALL(cudaMemcpy(dst, src, siz, cudaMemcpyHostToDevice))

#define DeviceToHost(dst, src, siz) CUDA_CALL(cudaMemcpy(dst, src, siz, cudaMemcpyDeviceToHost))

bool CUDAMiner::initDevice()
{
    cudalog << "Using Pci " << m_deviceDescriptor.uniqueId << ": " << m_deviceDescriptor.cuName
            << " (Compute " + m_deviceDescriptor.cuCompute + ") Memory : "
            << dev::getFormattedMemory((double)m_deviceDescriptor.totalMemory);

    // Set Hardware Monitor Info
    m_hwmoninfo.deviceType = HwMonitorInfoType::NVIDIA;
    m_hwmoninfo.devicePciId = m_deviceDescriptor.uniqueId;
    m_hwmoninfo.deviceIndex = -1;  // Will be later on mapped by nvml (see Farm() constructor)

    try
    {
        CUDA_CALL(cudaSetDevice(m_deviceDescriptor.cuDeviceIndex));
        CUDA_CALL(cudaDeviceReset());
    }
    catch (const cuda_runtime_error& ec)
    {
        cudalog << "Could not set CUDA device on Pci Id " << m_deviceDescriptor.uniqueId
                << " Error : " << ec.what();
        cudalog << "Mining aborted on this device.";
        return false;
    }
    return true;
}

void CUDAMiner::initEpoch()
{
    m_initialized = false;
    // If we get here it means epoch has changed so it's not necessary
    // to check again dag sizes. They're changed for sure
    m_current_target = 0;
    auto startInit = chrono::steady_clock::now();
    size_t RequiredTotalMemory = (m_epochContext.dagSize + m_epochContext.lightSize);
    size_t RequiredDagMemory = m_epochContext.dagSize;

    // Release the pause flag if any
    resume(MinerPauseEnum::PauseDueToInsufficientMemory);
    resume(MinerPauseEnum::PauseDueToInitEpochError);

    bool lightOnHost = false;
    try
    {
        hash128_t* dag;
        hash64_t* light;

        // If we have already enough memory allocated, we just have to
        // copy light_cache and regenerate the DAG
        if (m_allocated_memory_dag < m_epochContext.dagSize ||
            m_allocated_memory_light_cache < m_epochContext.lightSize)
        {
            // We need to reset the device and (re)create the dag
            // cudaDeviceReset() frees all previous allocated memory
            CUDA_CALL(cudaDeviceReset());
            CUDA_CALL(cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync));
            CUDA_CALL(cudaDeviceSetCacheConfig(cudaFuncCachePreferL1));

            // Check whether the current device has sufficient memory every time we recreate the dag
            if (m_deviceDescriptor.totalMemory < RequiredTotalMemory)
            {
                if (m_deviceDescriptor.totalMemory < RequiredDagMemory)
                {
                    cudalog << "Epoch " << m_epochContext.epochNumber << " requires "
                            << dev::getFormattedMemory((double)RequiredDagMemory) << " memory.";
                    cudalog << "This device hasn't enough memory available. Mining suspended ...";
                    pause(MinerPauseEnum::PauseDueToInsufficientMemory);
                    return;  // This will prevent to exit the thread and
                             // Eventually resume mining when changing coin or epoch (NiceHash)
                }
                else
                    lightOnHost = true;
            }

            cudalog << "Generating DAG + Light(on " << (lightOnHost ? "host" : "GPU")
                    << ") : " << dev::getFormattedMemory((double)RequiredTotalMemory);

            // create buffer for cache
            if (lightOnHost)
            {
                CUDA_CALL(
                    cudaHostAlloc((void**)&light, m_epochContext.lightSize, cudaHostAllocDefault));
                cudalog << "WARNING: Generating DAG will take minutes, not seconds";
            }
            else
                CUDA_CALL(cudaMalloc((void**)&light, m_epochContext.lightSize));
            m_allocated_memory_light_cache = m_epochContext.lightSize;
            CUDA_CALL(cudaMalloc((void**)&dag, m_epochContext.dagSize));
            m_allocated_memory_dag = m_epochContext.dagSize;

            // create mining buffers
            for (unsigned i = 0; i < m_deviceDescriptor.cuStreamSize; ++i)
            {
                CUDA_CALL(cudaMalloc(&m_search_buf[i], sizeof(Search_results)));
                CUDA_CALL(cudaStreamCreateWithFlags(&m_streams[i], cudaStreamNonBlocking));
            }
        }
        else
        {
            cudalog << "Generating DAG + Light (reusing buffers): "
                    << dev::getFormattedMemory((double)RequiredTotalMemory);
            get_constants(&dag, NULL, &light, NULL);
        }

        HostToDevice(light, m_epochContext.lightCache, m_epochContext.lightSize);

        set_constants(dag, m_epochContext.dagNumItems, light,
            m_epochContext.lightNumItems);  // in ethash_cuda_miner_kernel.cu

        ethash_generate_dag(
            m_epochContext.dagSize, m_block_multiple, m_deviceDescriptor.cuBlockSize, m_streams[0]);

        cudalog << "Generated DAG + Light in "
                << chrono::duration_cast<chrono::milliseconds>(
                       chrono::steady_clock::now() - startInit)
                       .count()
                << " ms. "
                << dev::getFormattedMemory(
                       lightOnHost ? (double)(m_deviceDescriptor.totalMemory - RequiredDagMemory) :
                                     (double)(m_deviceDescriptor.totalMemory - RequiredTotalMemory))
                << " left.";
    }
    catch (const cuda_runtime_error& ec)
    {
        cudalog << "Unexpected error " << ec.what() << " on CUDA device "
                << m_deviceDescriptor.uniqueId;
        cudalog << "Mining suspended ...";
        pause(MinerPauseEnum::PauseDueToInitEpochError);
    }

    m_initialized = true;
}

void CUDAMiner::workLoop()
{
    WorkPackage current;
    current.header = h256();

    if (!initDevice())
        return;

    try
    {
        while (!shouldStop())
        {
            const WorkPackage w = work();
            if (!w)
            {
                unique_lock<mutex> l(miner_work_mutex);
                m_new_work_signal.wait_for(l, chrono::seconds(3));
                continue;
            }

            // Epoch change ?
            if (current.epoch != w.epoch)
            {
                initEpoch();

                // As DAG generation takes a while we need to
                // ensure we're on latest job, not on the one
                // which triggered the epoch change
                current = w;
                continue;
            }

            // Persist most recent job.
            // Job's differences should be handled at higher level
            current = w;

            uint64_t upper64OfBoundary = (uint64_t)(u64)((u256)current.boundary >> 192);

            // adjust work multiplier
            float hr = RetrieveHashRate();
            if (hr >= 1e7)
                m_block_multiple =
                    uint32_t((hr * CU_TARGET_BATCH_TIME) /
                             (m_deviceDescriptor.cuStreamSize * m_deviceDescriptor.cuBlockSize));

            // Eventually start searching
            search(current.header.data(), upper64OfBoundary, current.startNonce, w);
        }

        // Reset miner and stop working
        CUDA_CALL(cudaDeviceReset());
    }
    catch (cuda_runtime_error const& _e)
    {
        string _what = "GPU error: ";
        _what.append(_e.what());
        throw runtime_error(_what);
    }
}

void CUDAMiner::kick_miner()
{
    static const uint32_t one = 1;
    if (!m_done)
    {
        m_done = true;
        for (unsigned i = 0; i < m_deviceDescriptor.cuStreamSize; i++)
            CUDA_CALL(cudaMemcpyAsync((uint8_t*)m_search_buf[i] + offsetof(Search_results, done),
                &one, sizeof(one), cudaMemcpyHostToDevice));
    }
}

int CUDAMiner::getNumDevices()
{
    int deviceCount;
    cudaError_t err = cudaGetDeviceCount(&deviceCount);
    if (err == cudaSuccess)
        return deviceCount;

    if (err == cudaErrorInsufficientDriver)
    {
        int driverVersion = 0;
        cudaDriverGetVersion(&driverVersion);
        if (driverVersion == 0)
            cwarn << "No CUDA driver found";
        else
            cwarn << "Insufficient CUDA driver " << to_string(driverVersion);
    }
    else
        cwarn << "CUDA Error : " << cudaGetErrorString(err);

    return 0;
}

void CUDAMiner::enumDevices(map<string, DeviceDescriptor>& _DevicesCollection)
{
    int numDevices = getNumDevices();

    for (int i = 0; i < numDevices; i++)
    {
        string uniqueId;
        ostringstream s;
        DeviceDescriptor deviceDescriptor;
        cudaDeviceProp props;

        try
        {
            size_t freeMem, totalMem;
            CUDA_CALL(cudaGetDeviceProperties(&props, i));
            CUDA_CALL(cudaMemGetInfo(&freeMem, &totalMem));
            s << setw(2) << setfill('0') << hex << props.pciBusID << ":" << setw(2)
              << props.pciDeviceID << ".0";
            uniqueId = s.str();

            if (_DevicesCollection.find(uniqueId) != _DevicesCollection.end())
                deviceDescriptor = _DevicesCollection[uniqueId];
            else
                deviceDescriptor = DeviceDescriptor();

            deviceDescriptor.name = string(props.name);
            deviceDescriptor.cuDetected = true;
            deviceDescriptor.uniqueId = uniqueId;
            deviceDescriptor.type = DeviceTypeEnum::Gpu;
            deviceDescriptor.cuDeviceIndex = i;
            deviceDescriptor.cuDeviceOrdinal = i;
            deviceDescriptor.cuName = string(props.name);
            deviceDescriptor.totalMemory = totalMem;
            deviceDescriptor.cuCompute =
                (to_string(props.major) + "." + to_string(props.minor));
            deviceDescriptor.cuComputeMajor = props.major;
            deviceDescriptor.cuComputeMinor = props.minor;
            deviceDescriptor.cuBlockSize = 128;
            deviceDescriptor.cuStreamSize = 2;


            _DevicesCollection[uniqueId] = deviceDescriptor;
        }
        catch (const cuda_runtime_error& _e)
        {
            cwarn << _e.what();
        }
    }
}

static const uint32_t zero3[3] = {0, 0, 0};  // zero the result count

void CUDAMiner::search(
    uint8_t const* header, uint64_t target, uint64_t start_nonce, const dev::eth::WorkPackage& w)
{
    set_header(*((const hash32_t*)header));
    if (m_current_target != target)
    {
        set_target(target);
        m_current_target = target;
    }
    uint32_t batch_blocks(m_block_multiple * m_deviceDescriptor.cuBlockSize);
    uint32_t stream_blocks(batch_blocks * m_deviceDescriptor.cuStreamSize);

    // prime each stream, clear search result buffers and start the search
    for (uint32_t streamIdx = 0; streamIdx < m_deviceDescriptor.cuStreamSize;
         streamIdx++, start_nonce += batch_blocks)
    {
        HostToDevice((uint8_t*)m_search_buf[streamIdx] + offsetof(Search_results, done), zero3,
            sizeof(zero3));
        run_ethash_search(m_block_multiple, m_deviceDescriptor.cuBlockSize, m_streams[streamIdx],
            m_search_buf[streamIdx], start_nonce);
    }

    m_done = false;
    uint32_t streams_bsy((1 << m_deviceDescriptor.cuStreamSize) - 1);

    // process stream batches until we get new work.

    while (streams_bsy)
    {
        if (!m_done)
            m_done = paused();

        uint32_t batchCount(0);

        // This inner loop will process each cuda stream individually
        for (uint32_t streamIdx = 0; streamIdx < m_deviceDescriptor.cuStreamSize;
             streamIdx++, start_nonce += batch_blocks)
        {
            uint32_t stream_mask(1 << streamIdx);
            if (!(streams_bsy & stream_mask))
                continue;

            cudaStream_t stream(m_streams[streamIdx]);
            uint8_t* buffer((uint8_t*)m_search_buf[streamIdx]);

            // Wait for the stream complete
            CUDA_CALL(cudaStreamSynchronize(stream));

            Search_results r;

            DeviceToHost(&r.counts, buffer + offsetof(Search_results, counts), sizeof(r.counts));

            // clear solution count, hash count and done
            HostToDevice(buffer + offsetof(Search_results, done), zero3, sizeof(zero3));

            r.counts.solCount = min(r.counts.solCount, MAX_SEARCH_RESULTS);
            batchCount += r.counts.hashCount;

            if (r.counts.solCount)
                DeviceToHost(&r.results, buffer + offsetof(Search_results, results),
                    r.counts.solCount * sizeof(Search_Result));

            if (m_done)
                streams_bsy &= ~stream_mask;
            else
                run_ethash_search(m_block_multiple, m_deviceDescriptor.cuBlockSize, stream,
                    (Search_results*)buffer, start_nonce);

            if (r.counts.solCount)
                for (uint32_t i = 0; i < r.counts.solCount; i++)
                {
                    uint64_t nonce(start_nonce - stream_blocks + r.results[i].gid);
                    h256 mix((::byte*)&r.results[i].mix, h256::ConstructFromPointer);

                    Farm::f().submitProof(
                        Solution{nonce, mix, w, chrono::steady_clock::now(), m_index});
                    cudalog << EthWhite << "Job: " << w.header.abridged()
                            << " Solution: " << toHex(nonce, HexPrefix::Add) << EthReset;
                }
            if (shouldStop())
                m_done = true;
        }
        updateHashRate(m_deviceDescriptor.cuBlockSize, batchCount);
    }

#ifdef DEV_BUILD
    // Optionally log job switch time
    if (!shouldStop() && (g_logOptions & LOG_SWITCH))
        cudalog << "Switch time: "
                << chrono::duration_cast<chrono::microseconds>(
                       chrono::steady_clock::now() - m_workSwitchStart)
                       .count()
                << " us.";
#endif
}
