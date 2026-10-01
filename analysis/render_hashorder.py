#!/usr/bin/env python3
"""(gamelua-render-spec) Reference model of lang::Hashtable<lang::String, GameLua::RenderObjectData>
as used by GameLua (this+0xb8) -- its bucket/chain iteration order IS the in-pass z-order of drawGame.

Reverse-engineered from:
  lang::String::hashCode            @0004bc74   h = s[0]; h = h*31 + (signed char)s[i]   (int32 wrap), "" -> 0
  Hashtable_getLargerInt            @00002cd4   37,67,131,257,521,1031,2053,4099,8209,16411,32771,65543,129403
  Hashtable<..>::getPair            @000192d8   bucket = (hash & 0x7fffffff) % cap
  Hashtable<..>::grow               @0001b538
  Hashtable<..>::remove             @0001b120
  Hashtable<..>::clear              @00019ea0   (keeps capacity AND overflow nodes)
  insertion sequence (createBoxLua @000129d0 / createCircleLua @00013034):
        if threshold <= count+1: grow()
        e = getPair(table, cap, name); e.value = ...; if !e.used: e.used = 1; count++

usage:  render_hashorder.py name1 name2 ...      -> prints iteration (draw) order after inserting in that order
"""
import sys

SIZES = [37, 67, 131, 257, 521, 1031, 2053, 4099, 8209, 16411, 32771, 65543, 129403]


def hash_code(s: str) -> int:
    b = s.encode('utf-8')
    if not b:
        return 0
    def sc(x):  # signed char
        return x - 256 if x > 127 else x
    h = sc(b[0])
    for c in b[1:]:
        h = (h * 31 + sc(c)) & 0xffffffff
    return h & 0xffffffff


def larger_int(n: int) -> int:
    for s in SIZES:
        if n < s:
            return s
    return (n + 129403) | 1


class Node:
    __slots__ = ('key', 'value', 'next', 'used')

    def __init__(self):
        self.key = ''
        self.value = None
        self.next = None
        self.used = False


class Hashtable:
    LOAD = 0.75

    def __init__(self):
        self.cap = 0
        self.table = []
        self.count = 0
        self.threshold = 0

    # --- getPair(table, cap, key): find, or claim a slot (does NOT set used / count) ---
    def _get_pair(self, table, cap, key):
        head = table[(hash_code(key) & 0x7fffffff) % cap]
        free = None
        e = head
        while e is not None:
            if e.used:
                if e.key == key:
                    return e
            else:
                free = e            # the LAST unused node of the chain wins
            e = e.next
        if free is None:
            free = Node()
            free.next = head.next   # new overflow node is linked right after the inline head
            head.next = free
        free.key = key
        free.value = None           # default RenderObjectData
        return free

    def _grow(self):
        ncap = larger_int(self.cap)
        ntab = [Node() for _ in range(ncap)]
        for head in self.table:     # old buckets in index order, each chain head-first
            e = head
            while e is not None:
                if e.used:
                    p = self._get_pair(ntab, ncap, e.key)
                    p.value = e.value
                    p.used = True
                e = e.next
        self.cap, self.table = ncap, ntab
        self.threshold = int(ncap * self.LOAD)

    def insert(self, key, value=None):
        if self.threshold <= self.count + 1:
            self._grow()
        e = self._get_pair(self.table, self.cap, key)
        e.value = value
        if not e.used:
            e.used = True
            self.count += 1

    def lookup(self, key):
        """setSprite/setTexture/... style lookup.  NOTE: like the original, a miss still claims
        (or allocates) an unused slot in the chain, which can perturb later insertion positions."""
        if self.cap < 1:
            return None
        e = self._get_pair(self.table, self.cap, key)
        return e if e.used else None

    def remove(self, key):
        if self.cap < 1:
            return
        head = self.table[(hash_code(key) & 0x7fffffff) % self.cap]
        prev, e = None, head
        while e is not None:
            nxt = e.next
            if e.used and e.key == key:
                e.used = False
                e.key = ''
                e.value = None
                self.count -= 1
                if e is not head:
                    prev.next = e.next     # overflow nodes are unlinked + freed, the inline head just becomes unused
            prev, e = e, nxt

    def clear(self):
        for head in self.table:
            e = head
            while e is not None:
                if e.used:
                    e.used = False
                    e.key = ''
                    e.value = None
                    self.count -= 1
                e = e.next

    def __iter__(self):
        """Iteration order used by drawGame / update / getBodyName."""
        for head in self.table:
            e = head
            while e is not None:
                if e.used:
                    yield e.key
                e = e.next


if __name__ == '__main__':
    h = Hashtable()
    for n in sys.argv[1:]:
        h.insert(n)
    for k in h:
        print('%-30s hash=%08x bucket=%d' % (k, hash_code(k), (hash_code(k) & 0x7fffffff) % h.cap))
