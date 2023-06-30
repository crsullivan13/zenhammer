#include "Memory/Memory.hpp"

#include <sys/mman.h>
#include <linux/mman.h>
#include <iostream>

#include "Utilities/Pagemap.hpp"

#define MMAP_PROT (PROT_READ | PROT_WRITE)
#define MMAP_FLAGS (MAP_SHARED | MAP_ANONYMOUS | MAP_POPULATE | MAP_HUGETLB | MAP_HUGE_1GB)

size_t Memory::get_max_superpages() {
  // try to allocate as many superpages as possible
  system("sudo bash -c 'echo 32 > tee /proc/sys/vm/nr_hugepages'");

  auto fp = popen("cat /proc/meminfo | grep HugePages_Total | tr -s ' ' | cut -d':' -f2 | xargs", "r");
  if (fp == nullptr) {
    Logger::log_error("Could not get the number of available superpages.");
    exit(EXIT_FAILURE);
  }

  std::string sout;
  if (fgets(sout.data(), 3, fp) != nullptr) {
    auto n = static_cast<size_t>(strtoul(sout.c_str(), nullptr, 10));
    Logger::log_info(format_string("Total #free superpages (HugePages_Total): %d", n));
    if (pclose(fp) < 0) {
      Logger::log_error("Closing popen file descriptor in get_max_superpages failed!");
      exit(EXIT_FAILURE);
    }
    return n;
  }

  return 0;
}

/// Allocates a MEM_SIZE bytes of memory by using super or huge pages.
void Memory::allocate_memory(size_t mem_size) {
  this->size = mem_size;
  volatile char *target = nullptr;
  FILE *fp;

  if (superpage) {
    // allocate memory using super pages
    fp = fopen(hugetlbfs_mountpoint.c_str(), "w+");
    if (fp==nullptr) {
      Logger::log_info(format_string("Could not mount superpage from %s. Error:", hugetlbfs_mountpoint.c_str()));
      Logger::log_data(std::strerror(errno));
      exit(EXIT_FAILURE);
    }
    auto mapped_target = mmap((void *) start_address, HUGEPAGE_SZ, MMAP_PROT, MMAP_FLAGS, fileno(fp), 0);
    if (mapped_target == MAP_FAILED) {
      perror("mmap");
      exit(EXIT_FAILURE);
    }
    target = (volatile char*) mapped_target;
    auto saddr_phy = pagemap::vaddr2paddr((uint64_t)mapped_target);
    Logger::log_info(format_string("Allocated memory (paddr): 0x%lx-0x%lx",
                                   (uint64_t)saddr_phy, (uint64_t)(saddr_phy+HUGEPAGE_SZ)));
  } else {
    // allocate memory using huge pages
    assert(posix_memalign((void **) &target, mem_size, mem_size)==0);
    assert(madvise((void *) target, mem_size, MADV_HUGEPAGE)==0);
    memset((char *) target, 'A', mem_size);
    // for khugepaged
    Logger::log_info("Waiting for khugepaged.");
    sleep(10);
  }

  if (target!=start_address) {
    Logger::log_error(format_string("Could not create mmap area at address %p, instead using %p.",
        start_address, target));
    start_address = target;
  }

  // initialize memory with random but reproducible sequence of numbers
  initialize(DATA_PATTERN::RANDOM);
}

void Memory::check_memory_full() {
//#if (DEBUG==1)
  // this function should only be used for debugging purposes as checking the whole superpage is expensive!
  Logger::log_debug("check_memory_full should only be used for debugging purposes as checking the whole superpage is expensive!");
  const auto pagesz = (size_t)getpagesize();
  bool has_flip = false;
  for (size_t i = 0; i < size; i += pagesz) {
    auto start_shadow = (volatile char *) ((uint64_t)shadow_page + i);
    auto start_sp = (volatile char *) ((uint64_t)start_address + i);
    if (memcmp((void*)start_shadow, (void*)start_sp, pagesz) != 0) {
      for (size_t j = 0; j < pagesz; j++) {
        if (start_shadow[j] != start_sp[j]) {
          auto addr = DRAMAddr((void*)&start_sp[j]).to_string_compact();
          Logger::log_error(format_string("Found bit flip in full memory scan at %p %s", &start_sp[j], addr.c_str()));
          has_flip = true;
        }
      }
    }
  }

  if (has_flip)
      exit(EXIT_FAILURE);
//#else
//  assert(false && "Memory::check_memory_full should only be used for debugging purposes!");
//#endif
}

void Memory::initialize(DATA_PATTERN patt) {
  if (not superpage) {
    Logger::log_error("The function Memory::initialize has not been adapted to work with regular pages! Exiting.");
    exit(EXIT_FAILURE);
  }

  this->data_pattern = patt;
  Logger::log_info("Initializing memory with pseudorandom sequence.");

  // for each page in the address space [start, end]
  for (uint64_t cur_page = 0; cur_page < HUGEPAGE_SZ; cur_page += getpagesize()) {
    // reseed rand to have a sequence of reproducible numbers, using this we can compare the initialized values with
    // those after hammering to see whether bit flips occurred
    reseed_srand(cur_page);
    for (uint64_t cur_pageoffset = 0; cur_pageoffset < (uint64_t) getpagesize(); cur_pageoffset += sizeof(int)) {
      // write (pseudo)random 4 bytes
      uint64_t offset = cur_page + cur_pageoffset;
      auto val = get_fill_value();
      *((uint32_t *) ((uint64_t)start_address + offset)) = val;
      *((uint32_t*)((uint64_t)shadow_page + offset)) = val;
    }
  }
}

void Memory::reseed_srand(uint64_t cur_page) {
  srand(cur_page*(uint64_t)getpagesize());
}

size_t Memory::check_memory(PatternAddressMapper &mapping, bool reproducibility_mode, bool verbose) {
  flipped_bits.clear();

  auto victim_rows = mapping.get_victim_rows();
  if (verbose) {
    Logger::log_info(format_string("Checking %zu victims for bit flips.", 
      victim_rows.size()));
  }

  size_t sum_found_bitflips = 0;
  for (auto &vr : victim_rows) {
    auto* start = (volatile char*)vr.to_virt();
    auto* end = start + DRAMAddr::get_row_to_row_offset();
    sum_found_bitflips += check_memory_internal(mapping, start, end, reproducibility_mode, verbose);
  }
  return sum_found_bitflips;
}

uint32_t Memory::get_fill_value() const {
  if (data_pattern == DATA_PATTERN::RANDOM) {
    return rand(); // NOLINT(cert-msc50-cpp)
  } else if (data_pattern == DATA_PATTERN::ZEROES) {
    return 0;
  } else if (data_pattern == DATA_PATTERN::ONES) {
    return std::numeric_limits<uint8_t>::max();
  } else {
    Logger::log_error("Could not initialize memory with given (unknown) DATA_PATTERN.");
    exit(EXIT_FAILURE);
  }
}

uint64_t Memory::round_down_to_next_page_boundary(uint64_t address) {
  const auto pagesize = getpagesize();
  return ((pagesize-1)&address)
      ? ((address+pagesize) & ~(pagesize-1))
      :address;
}

size_t Memory::check_memory_internal(PatternAddressMapper &mapping,
                                     const volatile char *start,
                                     const volatile char *end,
                                     bool reproducibility_mode,
                                     bool verbose) {
  // if end < start, then we flipped around the row list because we reached its end
  // in this case we use the typical row offset to 'guess' the next row
  if ((uint64_t)start > (uint64_t)end) {
    Logger::log_error("[Memory::check_memory_internal] start address cannot be larger than end address!");
    exit(EXIT_FAILURE);
  }

  // counter for the number of found bit flips in the memory region [start, end]
  size_t found_bitflips = 0;

  if (start==nullptr || end==nullptr || ((uint64_t) start > (uint64_t) end)) {
    Logger::log_error("Function check_memory called with invalid arguments.");
    Logger::log_data(format_string("Start addr.: %p, End addr.: %p", start, end));
    return found_bitflips;
  }

  auto start_offset_noalign = (uint64_t) (start - start_address);
  auto end_offset_noalign = (uint64_t) (end - start_address);
  const auto pagesize = static_cast<size_t>(getpagesize());

  // Round down start_offset, and round up end_offset.
  auto start_offset = (start_offset_noalign / pagesize) * pagesize;
  auto end_offset = ((end_offset_noalign + pagesize - 1) / pagesize) * pagesize; // ceiling division
  assert(start_offset <= start_offset_noalign);
  assert(end_offset >= end_offset_noalign);

  // for each page (4K) in the address space [start, end]
  for (uint64_t page_idx = start_offset; page_idx < end_offset; page_idx += pagesize) {
    // reseed rand to have the desired sequence of reproducible numbers
    reseed_srand(page_idx);

    uint64_t addr_superpage = ((uint64_t)start_address+page_idx);
    uint64_t addr_shadowpage = ((uint64_t)shadow_page+page_idx);

    // check if any bit flipped in the page using the fast memcmp function, if any flip occurred we need to iterate over
    // each byte one-by-one (much slower), otherwise we just continue with the next page
    if (memcmp((void*)addr_superpage, (void*)addr_shadowpage, pagesize) == 0)
      continue;

    // Iterate over blocks of 4 bytes (= sizeof(int)).
    for (uint64_t j = 0; j < (uint64_t) pagesize; j += sizeof(int)) {
      uint64_t offset_in_superpage = page_idx + j;
      volatile char *cur_addr = start_address + offset_in_superpage;
      volatile char *cur_shadow_addr = (volatile char*)shadow_page + offset_in_superpage;

      // if this address is outside the superpage we must not proceed to avoid segfault
      if ((uint64_t)cur_addr >= ((uint64_t)start_address+size))
        continue;

      // clear the cache to make sure we do not access a cached value
      clflushopt(cur_addr);
      mfence();

      // if the bit did not flip -> continue checking next block
      int expected_rand_value = *(int*)cur_shadow_addr;
      if (*((int *) cur_addr) == expected_rand_value)
        continue;

      // if the bit flipped -> compare byte-per-byte
      for (unsigned long c = 0; c < sizeof(int); c++) {
        volatile char *flipped_address = cur_addr + c;
        if (*flipped_address != ((char *) &expected_rand_value)[c]) {
          // auto simple_addr_flipped = conflict_cluster.get_simple_dram_address(flipped_address);
          auto simple_addr_flipped = DRAMAddr((void*)flipped_address);

          const auto flipped_addr_value = *(unsigned char *) flipped_address;
          const auto expected_value = ((unsigned char *) &expected_rand_value)[c];
          if (verbose) {
            Logger::log_bitflip(simple_addr_flipped, flipped_addr_value, expected_value);
          }
          // store detailed information about the bit flip
          BitFlip bitflip(simple_addr_flipped, (expected_value ^ flipped_addr_value), flipped_addr_value);
          // ..in the mapping that triggered this bit flip
          if (!reproducibility_mode) {
            if (mapping.bit_flips.empty()) {
              Logger::log_error("Cannot store bit flips found in given address mapping.\n"
                                "You need to create an empty vector in PatternAddressMapper::bit_flips before calling "
                                "check_memory.");
            }
            mapping.bit_flips.back().push_back(bitflip);
          }
          // ..in an attribute of this class so that it can be retrived by the caller
          flipped_bits.push_back(bitflip);
          found_bitflips += bitflip.count_bit_corruptions();
        }
      }

      // restore original (unflipped) value
      *((int *) cur_addr) = expected_rand_value;

      // flush this address so that value is committed before hammering there again
      clflushopt(cur_addr);
      mfence();
    }
  }
  
  return found_bitflips;
}

Memory::Memory(bool use_superpage)
    : size(0), superpage(use_superpage) {

  // allocate memory for the shadow page we will use later for fast comparison
  shadow_page = malloc(HUGEPAGE_SZ);
}

Memory::~Memory() {
  if (munmap((void *) start_address, size) != 0) {
    Logger::log_error("munmap failed with error:");
    Logger::log_data(std::strerror(errno));
    exit(EXIT_FAILURE);
  }
  start_address = nullptr;
  size = 0;

  free(shadow_page);
  shadow_page = nullptr;
}

volatile char *Memory::get_starting_address() const {
  return start_address;
}

std::string Memory::get_flipped_rows_text_repr() {
  std::stringstream ss;
  size_t cnt = 0;
  for (const auto &row : flipped_bits) {
    if (cnt > 0) {
      ss << ", ";
    }
    ss << row.address.get_row();
    cnt++;
  }
  return ss.str();
}
