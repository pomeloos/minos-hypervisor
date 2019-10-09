/*
 * Copyright (C) 2018 Min Le (lemin9538@gmail.com)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <minos/minos.h>
#include <virt/vm.h>
#include <minos/mmu.h>

extern unsigned char __el2_ttb0_pgd;
extern unsigned char __el2_ttb0_pud;
extern unsigned char __el2_ttb0_pmd_code;
extern unsigned char __el2_ttb0_pmd_io;

static DEFINE_SPIN_LOCK(mmap_lock);
static unsigned long hvm_normal_mmap_base = HVM_NORMAL_MMAP_START;
static size_t hvm_normal_mmap_size = HVM_NORMAL_MMAP_SIZE;
static unsigned long hvm_iomem_mmap_base = HVM_IO_MMAP_START;
static size_t hvm_iomem_mmap_size = HVM_IO_MMAP_SIZE;

static unsigned long alloc_pgd(void)
{
	/*
	 * return the table base address, this function
	 * is called when init the vm
	 *
	 * 2 pages for each VM to map 1T IPA memory
	 *
	 */
	void *page;

	page = __get_free_pages(GVM_PGD_PAGE_NR,
			GVM_PGD_PAGE_ALIGN);
	if (!page)
		panic("No memory to map vm memory\n");

	memset(page, 0, SIZE_4K * GVM_PGD_PAGE_NR);
	return (unsigned long)page;
}

void *vm_alloc_pages(struct vm *vm, int pages)
{
	struct page *page;
	struct mm_struct *mm = &vm->mm;

	page = alloc_pages(pages);
	if (!pages)
		return NULL;

	spin_lock(&vm->mm.lock);
	page->next = mm->head;
	mm->head = page;
	spin_unlock(&vm->mm.lock);

	return page_to_addr(page);
}

int create_guest_mapping(struct vm *vm, vir_addr_t vir,
		phy_addr_t phy, size_t size, unsigned long flags)
{
	unsigned long tmp;

	tmp = BALIGN(vir + size, PAGE_SIZE);
	vir = ALIGN(vir, PAGE_SIZE);
	phy = ALIGN(phy, PAGE_SIZE);
	size = tmp - vir;

	pr_debug("map 0x%x->0x%x size-0x%x vm-%d\n", vir,
			phy, size, vm->vmid);

	return create_mem_mapping(&vm->mm, vir, phy, size, flags);
}

static int __used destroy_guest_mapping(struct vm *vm,
		unsigned long vir, size_t size)
{
	unsigned long end;

	end = vir + size;
	end = BALIGN(end, PAGE_SIZE);
	vir = ALIGN(vir, PAGE_SIZE);
	size = end - vir;

	return destroy_mem_mapping(&vm->mm, vir, size, 0);
}

void release_vm_memory(struct vm *vm)
{
	struct mem_block *block, *n;
	struct mm_struct *mm;
	struct page *page, *tmp;

	if (!vm)
		return;

	mm = &vm->mm;
	page = mm->head;

	/*
	 * - release the block list
	 * - release the page table page and page table
	 * - set all the mm_struct to 0
	 * this function will not be called when vm is
	 * running, do not to require the lock
	 */
	list_for_each_entry_safe(block, n, &mm->block_list, list)
		release_mem_block(block);

	while (page != NULL) {
		tmp = page->next;
		release_pages(page);
		page = tmp;
	}

	free_pages((void *)mm->pgd_base);
	memset(mm, 0, sizeof(struct mm_struct));
}

unsigned long create_hvm_iomem_map(unsigned long phy, uint32_t size)
{
	unsigned long base = 0;
	struct vm *vm0 = get_vm_by_id(0);

	size = PAGE_BALIGN(size);

	spin_lock(&mmap_lock);
	if (hvm_iomem_mmap_size < size) {
		spin_unlock(&mmap_lock);
		goto out;
	}

	base = hvm_iomem_mmap_base;
	hvm_iomem_mmap_size -= size;
	hvm_iomem_mmap_base += size;
	spin_unlock(&mmap_lock);

	if (create_guest_mapping(vm0, base, phy, size, VM_RW))
		base = 0;
out:
	return base;
}

void destroy_hvm_iomem_map(unsigned long vir, uint32_t size)
{
	struct vm *vm0 = get_vm_by_id(0);

	/* just destroy the vm0's mapping entry */
	size = PAGE_BALIGN(size);
	destroy_guest_mapping(vm0, vir, size);
}

int vm_mmap_init(struct vm *vm, size_t memsize)
{
	int ret = -ENOMEM;

	spin_lock(&mmap_lock);
	if (hvm_normal_mmap_size < memsize)
		goto out;

	vm->mm.hvm_mmap_base = hvm_normal_mmap_base;
	hvm_normal_mmap_size -= memsize;
	hvm_normal_mmap_base += memsize;
	ret = 0;
out:
	spin_unlock(&mmap_lock);
	return ret;
}

/*
 * map VMx virtual memory to hypervisor memory
 * space to let hypervisor can access guest vm's
 * memory
 */
void *map_vm_mem(unsigned long gva, size_t size)
{
	unsigned long pa;

	/* assume the memory is continuously */
	pa = guest_va_to_pa(gva, 1);
	if (create_host_mapping(pa, pa, size, 0))
		return NULL;

	return (void *)pa;
}

void unmap_vm_mem(unsigned long gva, size_t size)
{
	unsigned long pa;

	/*
	 * what will happend if this 4k mapping is used
	 * in otherwhere
	 */
	pa = guest_va_to_pa(gva, 1);
	destroy_host_mapping(pa, size);
}

/*
 * map the guest vm memory space to vm0 to let vm0
 * can access all the memory space of the guest vm
 */
int vm_mmap(struct vm *vm, unsigned long offset, unsigned long size)
{
	unsigned long vir, phy, value;
	unsigned long *vm_pmd, *vm0_pmd;
	uint64_t attr;
	int i, vir_off, phy_off, count, left;
	struct vm *vm0 = get_vm_by_id(0);
	struct mm_struct *mm = &vm->mm;
	struct mm_struct *mm0 = &vm0->mm;
	struct memory_region *region = &mm->memory_regions[0];

	if (size > region->size)
		return -EINVAL;

	offset = ALIGN(offset, PMD_MAP_SIZE);
	size = BALIGN(size, PMD_MAP_SIZE);
	vir = region->vir_base + offset;
	phy = mm->hvm_mmap_base + offset;

	if ((offset + size) > region->free_size)
		size = (region->size - offset);

	left = size >> PMD_RANGE_OFFSET;
	phy_off = pmd_idx(phy);
	vm0_pmd = (unsigned long *)alloc_guest_pmd(mm0, phy);
	if (!vm0_pmd)
		return -ENOMEM;

	attr = page_table_description(VM_DES_BLOCK | VM_NORMAL);

	while (left > 0) {
		vm_pmd = (unsigned long *)get_mapping_pmd(mm->pgd_base, vir, 0);
		if (mapping_error(vm_pmd))
			return -EIO;

		vir_off = pmd_idx(vir);
		count = (PAGE_MAPPING_COUNT - vir_off);
		count = count > left ? left : count;

		for (i = 0; i < count; i++) {
			value = *(vm_pmd + vir_off);
			value &= PAGETABLE_ATTR_MASK;
			value |= attr;

			*(vm0_pmd + phy_off) = value;

			vir += PMD_MAP_SIZE;
			phy += PMD_MAP_SIZE;
			vir_off++;
			phy_off++;

			if ((phy_off & (PAGE_MAPPING_COUNT - 1)) == 0) {
				phy_off = 0;
				vm0_pmd = (unsigned long *)alloc_guest_pmd(mm0, phy);
				if (!vm0_pmd)
					return -ENOMEM;
			}
		}

		left -= count;
	}

	/* this function always run in vm0 */
	flush_local_tlb_guest();
	flush_icache_all();

	return 0;
}

void vm_unmmap(struct vm *vm)
{
	unsigned long phy;
	unsigned long *vm0_pmd;
	int left, count, offset;
	struct vm *vm0 = get_vm_by_id(0);
	struct mm_struct *mm0 = &vm0->mm;
	struct mm_struct *mm = &vm->mm;
	struct memory_region *region = &mm->memory_regions[0];

	phy = mm->hvm_mmap_base;
	left = region->size >> PMD_RANGE_OFFSET;

	while (left > 0) {
		vm0_pmd = (unsigned long *)get_mapping_pmd(mm0->pgd_base, phy, 0);
		if (mapping_error(vm0_pmd))
			return;

		offset = pmd_idx(phy);
		count = PAGE_MAPPING_COUNT - offset;
		if (count > left)
			count = left;

		memset((void *)(vm0_pmd + offset), 0,
				count * sizeof(unsigned long));

		if ((offset == 0) && (count == PAGE_MAPPING_COUNT)) {
			/* here we can free this pmd page TBD */
		}

		phy += count << PMD_RANGE_OFFSET;
		left -= count;
	}

	flush_local_tlb_guest();
}

/* alloc physical memory for guest vm */
int alloc_vm_memory(struct vm *vm)
{
	int i, count;
	unsigned long base;
	struct mm_struct *mm = &vm->mm;
	struct mem_block *block;
	struct memory_region *region = &mm->memory_regions[0];

	base = ALIGN(region->vir_base, MEM_BLOCK_SIZE);
	if (base != region->vir_base)
		pr_warn("memory base is not mem_block align\n");

	count = region->size >> MEM_BLOCK_SHIFT;

	/*
	 * here get all the memory block for the vm
	 * TBD: get contiueous memory or not contiueous ?
	 */
	for (i = 0; i < count; i++) {
		block = alloc_mem_block(GFB_VM);
		if (!block)
			goto free_vm_memory;

		// block->vmid = vm->vmid;
		list_add_tail(&mm->block_list, &block->list);
		region->free_size -= MEM_BLOCK_SIZE;
	}

	/*
	 * begin to map the memory for guest, actually
	 * this is map the ipa to pa in stage 2
	 */
	list_for_each_entry(block, &mm->block_list, list) {
		i = create_guest_mapping(vm, base, block->phy_base,
				MEM_BLOCK_SIZE, VM_NORMAL);
		if (i)
			goto free_vm_memory;

		base += MEM_BLOCK_SIZE;
	}

	return 0;

free_vm_memory:
	release_vm_memory(vm);

	return -ENOMEM;
}

phy_addr_t get_vm_memblock_address(struct vm *vm, unsigned long a)
{
	struct mm_struct *mm = &vm->mm;
	struct mem_block *block;
	unsigned long base = 0;
	struct memory_region *region = &mm->memory_regions[0];
	unsigned long offset = a - region->vir_base;

	if ((a < region->vir_base) || (a >= region->vir_base + region->size))
		return 0;

	list_for_each_entry(block, &mm->block_list, list) {
		if (offset == base)
			return block->phy_base;
		base += MEM_BLOCK_SIZE;
	}

	return 0;
}

void vm_mm_struct_init(struct vm *vm)
{
	struct mm_struct *mm = &vm->mm;

	init_list(&mm->block_list);
	mm->head = NULL;
	mm->pgd_base = 0;
	mm->nr_mem_regions = 0;
	spin_lock_init(&mm->lock);

	mm->pgd_base = alloc_pgd();
	if (mm->pgd_base == 0) {
		pr_err("No memory for vm page table\n");
		return;
	}

	/*
	 * for guest vm
	 *
	 * 0x0 - 0x3fffffff for vdev which controlled by
	 * hypervisor, like gic and timer
	 *
	 * 0x40000000 - 0x7fffffff for vdev which controlled
	 * by vm0, like virtio device, rtc device
	 */
	if (!vm_is_native(vm)) {
		mm->gvm_iomem_base = GVM_IO_MEM_START + SIZE_1G;
		mm->gvm_iomem_size = GVM_IO_MEM_SIZE - SIZE_1G;
	}
}

void vm_init_shmem(struct vm *vm, uint64_t base, uint64_t size)
{
	struct mm_struct *mm = &vm->mm;

	if (!vm_is_native(vm)) {
		pr_err("vm is not native vm can not init shmem\n");
		return;
	}

	pr_info("find shmem info for vm-%d 0x%x 0x%x\n",
			vm_id(vm), base, size);
	mm->shmem_base = base;
	mm->shmem_size = size;
}

void *vm_map_shmem(struct vm *vm, void *phy, uint32_t size,
		unsigned long flags)
{
	int ret;
	void *base;
	struct mm_struct *mm = &vm->mm;

	if (mm->shmem_size < size)
		return NULL;

	ret = create_guest_mapping(vm, (vir_addr_t)mm->shmem_base,
			(phy_addr_t)phy, size, flags);
	if (ret)
		return NULL;

	base = (void *)mm->shmem_base;
	mm->shmem_base += size;
	mm->shmem_size -= size;

	return base;
}

int vm_mm_init(struct vm *vm)
{
	int i, ret;
	struct memory_region *region;
	struct mm_struct *mm = &vm->mm;

	/* just mapping the physical memory for VM */
	for (i = 0; i < mm->nr_mem_regions; i++) {
		region = &mm->memory_regions[i];
		ret = create_guest_mapping(vm, region->vir_base,
				region->phy_base, region->size, 0);
		if (ret) {
			pr_err("build mem ma failed for vm-%d 0x%p 0x%p\n",
				vm->vmid, region->phy_base, region->size);
		}
	}

	return 0;
}
