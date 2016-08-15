#ifndef SPLITORDERED_HASHTABLE_H_
#define SPLITORDERED_HASHTABLE_H_

#include <cassert>
#include <cstdlib>

#include <atomic>
#include <array>

#include "defs.h"
#include "listops.h"
#include "utils.h"

namespace splitordered {

template<class T>
class doubling_list {
 public:
  doubling_list() {
    T* null_ptr = NULL;
    for (auto& x : buckets_)
      x = null_ptr;
    buckets_[0] = new T[1];
    buckets_[1] = new T[1];
    num_populated_buckets_.store(2);
  }

  ~doubling_list() {
    for (auto& x : buckets_)
      delete[] x;
  }

  T& operator[](uint32_t i) {
    uint32_t pos = i + 1;
    uint32_t bucket_idx = slog::bit_utils::highest_bit(pos);
    uint32_t bucket_off = pos ^ (1 << bucket_idx);
    return buckets_[bucket_idx][bucket_off];
  }

  size_t size() {
    uint32_t n = num_populated_buckets_.load();
    return 0x01 << (n - 1);
  }

  size_t double_size(size_t expected_size) {
    uint32_t n = num_populated_buckets_.load();
    uint32_t s = 0x01 << (n - 1);
    if (expected_size == s) {
      T* new_bucket = new T[s];
      T* null_ptr = NULL;

      // Only one thread will be successful in replacing the NULL reference with newly
      // allocated bucket.
      if (std::atomic_compare_exchange_weak(&buckets_[n], &null_ptr,
                                            new_bucket)) {
        num_populated_buckets_.fetch_add(1);
      } else {
        delete[] new_bucket;
      }
    }
    return size();
  }

 private:
  std::atomic<uint32_t> num_populated_buckets_;
  std::array<std::atomic<T*>, 32> buckets_;
};

template<class data_type>
class hash_table {
 public:
  static const int32_t MAX_LOAD = 4;

  hash_table() {
    count.store(0);
    {
      hash_entry<data_type> *dummy = new hash_entry<data_type>();
      assert(dummy);
      buckets_[0] = CONSTRUCT(0, dummy);
    }
  }

  ~hash_table() {
    marked_ptr_t cursor;
    cursor = buckets_[0];
    while (PTR_OF(cursor) != NULL) {
      marked_ptr_t tmp = cursor;
      assert(MARK_OF(tmp) == 0);
      cursor = PTR_OF(cursor)->next;
      free(PTR_OF(tmp));
    }
  }

  bool put(const key_t key, const data_type value) {
    hash_entry<data_type> *node = new hash_entry<data_type>();  // XXX: should pull out of a memory pool
    size_t bucket;
    uint64_t lkey = key;

    lkey = hashword(lkey);
    bucket = lkey % buckets_.size();

    fprintf(stderr, "#Buckets = %zu, bucket_idx=%zu\n", buckets_.size(), bucket);

    assert(node);
    assert((lkey & MSB) == 0);
    node->key = regularkey(lkey);
    node->value = value;
    node->next = UNINITIALIZED;

    if (buckets_[bucket] == UNINITIALIZED)
      initialize_bucket(bucket);

    if (!list_ops<data_type>::insert(&(buckets_[bucket]), node, NULL)) {
      free(node);
      return false;
    }

    size_t csize = buckets_.size();
    if (count.fetch_add(1) / csize > MAX_LOAD) {
      // double size
      size_t dsize = buckets_.double_size(csize);
      assert(dsize >= csize * 2);
    }
    return true;
  }

  bool get(const key_t key, data_type* value) {
    size_t bucket;
    uint64_t lkey = key;

    lkey = hashword(lkey);
    bucket = lkey % buckets_.size();

    if (buckets_[bucket] == UNINITIALIZED)
      initialize_bucket(bucket);

    return list_ops<data_type>::find(&(buckets_[bucket]), regularkey(lkey),
                                     value,
                                     NULL,
                                     NULL,
                                     NULL);
  }

  bool remove(const key_t key) {
    size_t bucket;
    uint64_t lkey = key;

    lkey = hashword(lkey);
    bucket = lkey % buckets_.size();

    if (buckets_[bucket] == UNINITIALIZED)
      initialize_bucket(bucket);

    if (!list_ops<data_type>::remove(&(buckets_[bucket]), regularkey(lkey)))
      return false;

    count.fetch_add(-1);
    return true;
  }

 private:
  void initialize_bucket(size_t bucket) {
    size_t parent = get_parent(bucket);
    marked_ptr_t cur;

    if (buckets_[parent] == UNINITIALIZED) {
      initialize_bucket(parent);
    }

    hash_entry<data_type> *dummy = new hash_entry<data_type>();  // XXX: should pull out of a memory pool
    assert(dummy);
    dummy->key = dummykey(bucket);
    dummy->value = INVALID(data_type);
    dummy->next = UNINITIALIZED;
    if (!list_ops<data_type>::insert(&(buckets_[parent]), dummy, &cur)) {
      free(dummy);
      dummy = PTR_OF(cur);
      while (buckets_[bucket] != CONSTRUCT(0, dummy))
        ;
    } else {
      buckets_[bucket] = CONSTRUCT(0, dummy);
    }
  }

  inline size_t get_parent(uint64_t bucket) {
    uint64_t t = bucket;
    t |= t >> 1;
    t |= t >> 2;
    t |= t >> 4;
    t |= t >> 8;
    t |= t >> 16;
    t |= t >> 32;     // creates a mask
    return bucket & (t >> 1);
  }

  inline so_key_t regularkey(const key_t key) {
    return REVERSE(key | MSB);
  }

  inline so_key_t dummykey(const key_t key) {
    return REVERSE(key);
  }

  /* this function based on http://burtleburtle.net/bob/hash/evahash.html */
  inline uint64_t hashword(uint64_t key) { /*{{{*/
    uint32_t a, b, c;

    const union {
      uint64_t key;
      uint8_t b[sizeof(uint64_t)];
    } k = { key };

    a = b = c = 0x32533d0c + sizeof(uint64_t);  // an arbitrary value, randomly selected
    c += 47;

    b += k.b[7] << 24;
    b += k.b[6] << 16;
    b += k.b[5] << 8;
    b += k.b[4];
    a += k.b[3] << 24;
    a += k.b[2] << 16;
    a += k.b[1] << 8;
    a += k.b[0];

    c ^= b;
    c -= rot(b, 14);
    a ^= c;
    a -= rot(c, 11);
    b ^= a;
    b -= rot(a, 25);
    c ^= b;
    c -= rot(b, 16);
    a ^= c;
    a -= rot(c, 4);
    b ^= a;
    b -= rot(a, 14);
    c ^= b;
    c -= rot(b, 24);
    return ((uint64_t) c + (((uint64_t) b) << 32)) & (~MSB);
  }

  doubling_list<marked_ptr_t> buckets_;
  std::atomic<int64_t> count;
};

}

#endif /* SPLITORDERED_HASHTABLE_H_ */