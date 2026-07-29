import re

class _childplusextras_iterator():
    def __init__(self, extras, elems, usedCount):
        self.extras = extras
        self.elems = elems
        self.usedCount = usedCount
        self.i = 0

    def __iter__(self):
        return self

    def __next__(self):
        if len(self.extras) > 0:
            return self.extras.pop(0)

        if self.i >= self.usedCount:
            raise StopIteration
        ret = ('[{}]'.format(self.i), self.elems[self.i])

        self.i = self.i + 1
        return ret

class nbdstrPrinter(object):
    def __init__(self, val):
        self.val = val

    def to_string(self):
        if self.val['d']['fixed']['flags'] & int(self.val['FIXED_STATE']):
            return self.val['d']['fixed']['str']
        if self.val['d']['fixed']['flags'] & int(self.val['ALLOC_STATE']):
            return self.val['d']['alloc']['str']
        return self.val['d']['arr']['str'].cast(gdb.lookup_type("char").pointer())

    def display_hint(self):
        return 'string'

class nbdinflexiblestrPrinter(object):
    def __init__(self, val):
        self.val = val

    def to_string(self):
        return self.val['pointer'].cast(gdb.lookup_type("char").pointer())

    def display_hint(self):
        return 'string'

class nbdarrayPrinter(object):
    def __init__(self, val):
        self.val = val

    def children(self):
        return _childplusextras_iterator([], self.val['elems'], self.val['usedCount'])

    def to_string(self):
        return None

    def display_hint(self):
        return 'array'

class nbdpairPrinter(object):
    def __init__(self, val):
        self.val = val

    def children(self):
        return [('first', self.val['first']), ('second', self.val['second'])]

    def to_string(self):
        return ('{{ {}, {} }}'.format(self.val['first'], self.val['second']))

class nbdbytetrieNodePrinter(object):
    def __init__(self, val):
        self.val = val
        self.nodeSize = self.val['v']['_trie'] & int(0xC000)
        self.trietype = self.val.type.name.replace('::NodeOrLeaf', '')

    def children(self):
        ret = []

        prefixLen = self.val['v']['_trie'] & int(0x1FFF)
        prefixName = '{} byte prefix'.format(prefixLen)
        thisPtr = int(self.val.address)
        prefixValue = thisPtr + self.val.type.sizeof
        prefixValue = gdb.Value(prefixValue).cast(gdb.lookup_type("byte").pointer())

        ret.append((prefixName, prefixValue))

        if self.val['v']['_trie'] & int(0x2000):
            ret.append(('value', self.val['v']))

        if self.nodeSize == 0xC000:
            ret.append(('children', gdb.Value(thisPtr).cast(gdb.lookup_type(self.trietype + "::FatNode").pointer()).dereference()['children']))
        elif self.nodeSize == 0x8000:
            ret.append(('children', gdb.Value(thisPtr).cast(gdb.lookup_type(self.trietype + "::SmallNode<(unsigned char)'\\b'>").pointer()).dereference()['children']))
            ret.append(('childBytes', gdb.Value(thisPtr).cast(gdb.lookup_type(self.trietype + "::SmallNode<(unsigned char)'\\b'>").pointer()).dereference()['childBytes']))
        elif self.nodeSize == 0x4000:
            ret.append(('children', gdb.Value(thisPtr).cast(gdb.lookup_type(self.trietype + "::SmallNode<(unsigned char)'\\x02'>").pointer()).dereference()['children']))
            ret.append(('childBytes', gdb.Value(thisPtr).cast(gdb.lookup_type(self.trietype + "::SmallNode<(unsigned char)'\\x02'>").pointer()).dereference()['childBytes']))

        return ret

    def to_string(self):
        if self.val['v']['_trie'] & int(0x2000):
            if self.nodeSize == 0xC000:
                return 'FatNode = {}'.format(self.val['v'])
            elif self.nodeSize == 0x8000:
                return 'Node8 = {}'.format(self.val['v'])
            elif self.nodeSize == 0x4000:
                return 'Node2 = {}'.format(self.val['v'])
        else:
            if self.nodeSize == 0xC000:
                return 'FatNode without value'
            elif self.nodeSize == 0x8000:
                return 'Node8 without value'
            elif self.nodeSize == 0x4000:
                return 'Node2 without value'

        return 'Leaf = {}'.format(self.val['v'])


class sdtypePrinter(object):
    def __init__(self, val):
        self.val = val

    def children(self):
        return [
                ('name', self.val['name']),
                ('basetype', self.val['basetype']),
                ('byteSize', self.val['byteSize']),
                ('flags', self.val['flags']),
               ]

    def to_string(self):
        return str(self.val['name'])

class sdobjectPrinter(object):
    def __init__(self, val):
        self.val = val

    def children(self):
        if (self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::Array'].enumval) or \
            self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::Struct'].enumval)):
            return _childplusextras_iterator([], self.val['data']['children']['elems'], self.val['data']['children']['usedCount'])
        else:
            return [
                    ('type', self.val['type']),
                    ('name', self.val['name']),
                    ('data', self.val['data']),
                   ]

    def to_string(self):
        if self.val['type']['flags'] & int(gdb.lookup_type('SDTypeFlags')['SDTypeFlags::HasCustomString'].enumval):
            return '{} = {}'.format(self.val['name'], self.val['data']['str'])
        if self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::String'].enumval):
            return '{} = {}'.format(self.val['name'], self.val['data']['str'])
        if self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::UnsignedInteger'].enumval):
            return '{} = {}'.format(self.val['name'], self.val['data']['u'])
        if self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::SignedInteger'].enumval):
            return '{} = {}'.format(self.val['name'], self.val['data']['i'])
        if self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::Float'].enumval):
            return '{} = {}'.format(self.val['name'], self.val['data']['d'])
        if self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::Boolean'].enumval):
            return '{} = {}'.format(self.val['name'], self.val['data']['b'])
        if self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::Character'].enumval):
            return '{} = {}'.format(self.val['name'], self.val['data']['c'])
        if self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::Resource'].enumval):
            return '{} = {}'.format(self.val['name'], self.val['data']['id'])
        if self.val['type']['basetype'] == int(gdb.lookup_type('SDBasic')['SDBasic::Array'].enumval):
            return '{} = {}[]'.format(self.val['name'], self.val['type']['name'])
        return 'SDObject: {} {}'.format(self.val['type']['name'], self.val['name'])


class sdchunkPrinter(object):
    def __init__(self, val):
        self.val = val

    def children(self):
        return _childplusextras_iterator([('metadata', self.val['metadata'])], self.val['data']['children']['elems'], self.val['data']['children']['usedCount'])

    def to_string(self):
        return ('SDChunk: {} ({})'.format(self.val['name'], self.val['metadata']['chunkID']))

class sdchunkPtrPrinter(sdchunkPrinter):
    def __init__(self, val):
        self.val = val.dereference()

class sdobjectPtrPrinter(sdobjectPrinter):
    def __init__(self, val):
        self.val = val.dereference()

class nbdbytetrieNodePtrPrinter(nbdbytetrieNodePrinter):
    def __init__(self, val):
        self.val = val.dereference()
        self.nodeSize = self.val['v']['_trie'] & int(0xC000)
        self.trietype = self.val.type.name.replace('::NodeOrLeaf', '')

import gdb.printing

def register(objfile):
    """Register the pretty printers within the given objfile."""

    printer = gdb.printing.RegexpCollectionPrettyPrinter('noobdawn')

    printer.add_printer('nbdstr', r'^nbdstr$', nbdstrPrinter)
    printer.add_printer('nbdinflexiblestr', r'^nbdinflexiblestr$', nbdinflexiblestrPrinter)
    printer.add_printer('nbdarray', r'^nbdarray<.*>$', nbdarrayPrinter)
    printer.add_printer('nbdpair', r'^nbdpair<.*>$', nbdpairPrinter)
    printer.add_printer('nbdbytetrie', r'^nbdbytetrie<.*>::NodeOrLeaf$', nbdbytetrieNodePrinter)

    printer.add_printer('SDType', r'^SDType$', sdtypePrinter)
    printer.add_printer('SDObject', r'^SDObject$', sdobjectPrinter)
    printer.add_printer('SDChunk', r'^SDChunk$', sdchunkPrinter)

    printer.add_printer('SDObjectPtr', r'^SDObject *\*&?$', sdobjectPtrPrinter)
    printer.add_printer('SDChunkPtr', r'^SDChunk *\*&?$', sdchunkPtrPrinter)
    printer.add_printer('nbdbytetriePtr', r'^nbdbytetrie<.*>::NodeOrLeaf *\*&?$', nbdbytetrieNodePtrPrinter)

    if objfile == None:
        objfile = gdb

    gdb.printing.register_pretty_printer(objfile, printer)

register(gdb.current_objfile())
