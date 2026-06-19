#pragma once
#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#endif

/* See Intel SDM Table 24-8: Format of Extended-Page-Table Pointer */
typedef union _EPTP {
  uint64_t all;
  struct {
    uint64_t memory_type : 3;      /* bits 2:0  — 0=UC, 6=WB */
    uint64_t page_walk_length : 3; /* bits 5:3  — value is (walk depth - 1) */
    uint64_t dirty_access_enabled
        : 1;                    /* bit  6    — enable A/D flags for EPT */
    uint64_t reserved1 : 5;     /* bits 11:7 */
    uint64_t pml4_address : 36; /* bits 47:12 — page frame number of EPT PML4 */
    uint64_t reserved2 : 16;    /* bits 63:48 */
  } fields;
} EPTP;

/* See Intel SDM Table 28-1: Format of an EPT PML4 Entry */
typedef union _EPT_PML4E {
  uint64_t all;
  struct {
    uint64_t read : 1;      /* bit  0    — read access */
    uint64_t write : 1;     /* bit  1    — write access */
    uint64_t execute : 1;   /* bit  2    — execute access (supervisor-mode) */
    uint64_t reserved1 : 5; /* bits 7:3  — must be zero */
    uint64_t accessed : 1;  /* bit  8    — set by hardware when entry is used */
    uint64_t ignored1 : 1;  /* bit  9    */
    uint64_t execute_for_usermode
        : 1; /* bit  10   — execute for user-mode linear addresses */
    uint64_t ignored2 : 1; /* bit  11   */
    uint64_t physical_address
        : 36;               /* bits 47:12 — PFN of the next-level EPT table */
    uint64_t reserved2 : 4; /* bits 51:N */
    uint64_t ignored3 : 12; /* bits 63:52 */
  } fields;
} EPT_PML4E;

/* See Intel SDM Table 28-3: Format of an EPT Page-Directory-Pointer-Table Entry
 */
typedef union _EPT_PDPTE {
  uint64_t all;
  struct {
    uint64_t read : 1;
    uint64_t write : 1;
    uint64_t execute : 1;
    uint64_t reserved1 : 5; /* bits 7:3 — must be zero (bit 7 = 0 means this
                               entry references a PD, not a 1GB page) */
    uint64_t accessed : 1;
    uint64_t ignored1 : 1;
    uint64_t execute_for_usermode : 1;
    uint64_t ignored2 : 1;
    uint64_t physical_address : 36; /* PFN of EPT PD */
    uint64_t reserved2 : 4;
    uint64_t ignored3 : 12;
  } fields;
} EPT_PDPTE;

/* See Intel SDM Table 28-5: Format of an EPT Page-Directory Entry */
typedef union _EPT_PDE {
  uint64_t all;
  struct {
    uint64_t read : 1;
    uint64_t write : 1;
    uint64_t execute : 1;
    uint64_t reserved1
        : 5; /* bits 7:3 — bit 7 = 0 means this entry references a PT */
    uint64_t accessed : 1;
    uint64_t ignored1 : 1;
    uint64_t execute_for_usermode : 1;
    uint64_t ignored2 : 1;
    uint64_t physical_address : 36; /* PFN of EPT PT */
    uint64_t reserved2 : 4;
    uint64_t ignored3 : 12;
  } fields;
} EPT_PDE;

/* See Intel SDM Table 28-6: Format of an EPT Page-Table Entry */
typedef union _EPT_PTE {
  uint64_t all;
  struct {
    uint64_t read : 1;    /* bit  0 */
    uint64_t write : 1;   /* bit  1 */
    uint64_t execute : 1; /* bit  2 */
    uint64_t ept_memory_type
        : 3; /* bits 5:3  — memory type for this page (UC=0, WB=6) */
    uint64_t ignore_pat
        : 1; /* bit  6    — if set, ignore the guest's PAT MSR for this page */
    uint64_t ignored1 : 1;      /* bit  7 */
    uint64_t accessed_flag : 1; /* bit  8 */
    uint64_t dirty_flag : 1; /* bit  9    — set on write to the mapped page */
    uint64_t execute_for_usermode : 1; /* bit  10 */
    uint64_t ignored2 : 1;             /* bit  11 */
    uint64_t physical_address
        : 36; /* bits 47:12 — PFN of the actual host physical page */
    uint64_t reserved : 4;  /* bits 51:N */
    uint64_t ignored3 : 11; /* bits 62:52 */
    uint64_t suppress_ve
        : 1; /* bit  63   — suppress EPT-violation (#VE) for this page */
  } fields;
} EPT_PTE;

/* ---- Memory type constants (from MTRR/PAT) ---- */
#define MEMORY_TYPE_UNCACHEABLE     0
#define MEMORY_TYPE_WRITE_COMBINING 1
#define MEMORY_TYPE_WRITE_THROUGH   4
#define MEMORY_TYPE_WRITE_PROTECT   5
#define MEMORY_TYPE_WRITE_BACK      6

#define MAX_MTRR_RANGES 64

/* MTRR register layouts */
typedef union {
  uint64_t all;
  struct {
    uint64_t type              : 8;
    uint64_t reserved1         : 4;
    uint64_t page_frame_number : 40;
    uint64_t reserved2         : 12;
  } fields;
} IA32_MTRR_PHYSBASE_REGISTER;

typedef union {
  uint64_t all;
  struct {
    uint64_t valid             : 1;
    uint64_t reserved1         : 11;
    uint64_t page_frame_number : 40;
    uint64_t reserved2         : 12;
  } fields;
} IA32_MTRR_PHYSMASK_REGISTER;

typedef union {
  uint64_t all;
  struct {
    uint64_t variable_range_count : 8;
    uint64_t fixed_range_support  : 1;
    uint64_t reserved1            : 55;
  } fields;
} IA32_MTRR_CAPABILITIES_REGISTER;

/* A single MTRR variable range descriptor */
typedef struct {
  uint64_t PhysicalBaseAddress;
  uint64_t PhysicalEndAddress;
  uint8_t  MemoryType;
} MTRR_RANGE_DESCRIPTOR;

/* IA32_MTRR_DEF_TYPE MSR layout */
typedef union {
  uint64_t all;
  struct {
    uint64_t default_memory_type : 3;
    uint64_t reserved1           : 7;
    uint64_t fixed_range_enable  : 1;
    uint64_t mtrr_enable         : 1;
    uint64_t reserved2           : 52;
  } fields;
} IA32_MTRR_DEF_TYPE_REGISTER;

/* ---- EPT page table dimensions ---- */
#define VMM_EPT_PML4E_COUNT 512
#define VMM_EPT_PML3E_COUNT 512
#define VMM_EPT_PML2E_COUNT 512
#define VMM_EPT_PML1E_COUNT 512
#define SIZE_2_MB (2 * 1024 * 1024ULL)

/* Address masks for indexing into EPT tables */
#define ADDRMASK_EPT_PML1_INDEX(var) (((var) & 0x1FF000ULL) >> 12)
#define ADDRMASK_EPT_PML2_INDEX(var) (((var) & 0x3FE00000ULL) >> 21)
#define ADDRMASK_EPT_PML3_INDEX(var) (((var) & 0x7FC0000000ULL) >> 30)
#define ADDRMASK_EPT_PML4_INDEX(var) (((var) & 0xFF8000000000ULL) >> 39)

/* PML2 entry when used as a 2 MB large page */
typedef union _EPT_PML2_ENTRY {
  uint64_t all;
  struct {
    uint64_t read              : 1;
    uint64_t write             : 1;
    uint64_t execute           : 1;
    uint64_t reserved1         : 5;
    uint64_t accessed          : 1;
    uint64_t ignored1          : 1;
    uint64_t execute_for_usermode : 1;
    uint64_t ignored2          : 1;
    uint64_t memory_type       : 3;
    uint64_t ignore_pat        : 1;
    uint64_t large_page        : 1;
    uint64_t reserved2         : 4;
    uint64_t page_frame_number : 26;
    uint64_t reserved3         : 17;
  } fields;
} EPT_PML2_ENTRY;

/* PML2 entry when used as a pointer to a PML1 table (after splitting) */
typedef union _EPT_PML2_POINTER {
  uint64_t all;
  struct {
    uint64_t read              : 1;
    uint64_t write             : 1;
    uint64_t execute           : 1;
    uint64_t reserved1         : 5;
    uint64_t accessed          : 1;
    uint64_t ignored1          : 1;
    uint64_t execute_for_usermode : 1;
    uint64_t ignored2          : 1;
    uint64_t physical_address  : 36;
    uint64_t reserved2         : 4;
    uint64_t ignored3          : 12;
  } fields;
} EPT_PML2_POINTER;

/* PML1 entry — 4 KB page */
typedef union _EPT_PML1_ENTRY {
  uint64_t all;
  struct {
    uint64_t read              : 1;
    uint64_t write             : 1;
    uint64_t execute           : 1;
    uint64_t reserved1         : 5;
    uint64_t accessed          : 1;
    uint64_t dirty             : 1;
    uint64_t execute_for_usermode : 1;
    uint64_t ignored1          : 1;
    uint64_t memory_type       : 3;
    uint64_t ignore_pat        : 1;
    uint64_t ignored2          : 1;
    uint64_t physical_address  : 36;
    uint64_t reserved2         : 4;
    uint64_t ignored3          : 11;
    uint64_t suppress_ve       : 1;
  } fields;
} EPT_PML1_ENTRY;

/* INVEPT descriptor (Intel SDM 30.3) */
typedef struct {
  uint64_t ept_pointer;
  uint64_t reserved;
} INVEPT_DESCRIPTOR;

typedef enum {
  INVEPT_SINGLE_CONTEXT = 1,
  INVEPT_ALL_CONTEXTS   = 2
} INVEPT_TYPE;

#ifdef __KERNEL__
#include <linux/list.h>

/* Dynamic split: holds the PML1 table for a split 2 MB page */
typedef struct _VMM_EPT_DYNAMIC_SPLIT {
  EPT_PML1_ENTRY PML1[VMM_EPT_PML1E_COUNT];
  EPT_PML2_ENTRY *Entry;
  struct list_head DynamicSplitList;
} VMM_EPT_DYNAMIC_SPLIT;

/* The full EPT page table structure */
typedef struct VMM_EPT_PAGE_TABLE {
  EPT_PML4E      PML4[VMM_EPT_PML4E_COUNT];
  EPT_PDPTE      PML3[VMM_EPT_PML3E_COUNT];
  EPT_PML2_ENTRY PML2[VMM_EPT_PML3E_COUNT][VMM_EPT_PML2E_COUNT];
  struct list_head DynamicSplitList;
} VMM_EPT_PAGE_TABLE;

/* Global EPT state */
typedef struct {
  MTRR_RANGE_DESCRIPTOR MemoryRanges[MAX_MTRR_RANGES];
  uint32_t NumberOfEnabledMemoryRanges;
  VMM_EPT_PAGE_TABLE *EptPageTable;
  EPTP EptPointer;
} EPT_STATE;

extern EPT_STATE *g_ept_state;
#endif
