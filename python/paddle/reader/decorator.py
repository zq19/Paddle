# Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

__all__ = [
    'cache', 'map_readers', 'buffered', 'compose', 'chain', 'shuffle',
    'ComposeNotAligned', 'firstn', 'xmap_readers', 'multiprocess_reader'
]

from threading import Thread
import subprocess
import multiprocessing
import six
import sys

from six.moves.queue import Queue
from six.moves import zip_longest
from six.moves import map
from six.moves import zip
import itertools
import random
import zlib
import paddle.compat as cpt


def cache(reader):
    """
    Cache the reader data into memory. 

    Be careful that this method may take long time to process, 
    and consume lots of memory. :code:`reader()` would only 
    call once. 

    Args:
        reader (generator): a reader object which yields 
            data each time.

    Returns:
        generator: a decorated reader object which yields data from cached memory.
    """
    all_data = tuple(reader())

    def __impl__():
        for item in all_data:
            yield item

    return __impl__


def map_readers(func, *readers):
    """
    Creates a data reader that outputs return value of function using
    output of each data reader as arguments.

    If input readers output the following data entries: 2 3,
    and the input func is mul(x, y),
    the output of the resulted reader will be 6.


    Args:
        func: a function to read data and compute result, the output of this function 
              will be set as the output of the resulted data reader.
        readers (Reader|list of Reader): list of readers whose outputs will be used as arguments of func.
 
    Returns:
        the resulted data reader (Reader)

    Examples:

        .. code-block:: python

         import paddle.reader
         d = {"h": 0, "i": 1}
         def func(x):
             return d[x]
         def reader():
             yield "h"
             yield "i"
         map_reader_result = paddle.reader.map_readers(func, reader)
    """

    def reader():
        rs = []
        for r in readers:
            rs.append(r())
        for e in map(func, *rs):
            yield e

    return reader


def shuffle(reader, buf_size):
    """
    Creates a data reader whose data output is shuffled.

    Output from the iterator that created by original reader will be
    buffered into shuffle buffer, and then shuffled. The size of shuffle buffer
    is determined by argument buf_size.

    :param reader: the original reader whose output will be shuffled.
    :type reader: callable
    :param buf_size: shuffle buffer size.
    :type buf_size: int

    :return: the new reader whose output is shuffled.
    :rtype: callable
    """

    def data_reader():
        buf = []
        for e in reader():
            buf.append(e)
            if len(buf) >= buf_size:
                random.shuffle(buf)
                for b in buf:
                    yield b
                buf = []

        if len(buf) > 0:
            random.shuffle(buf)
            for b in buf:
                yield b

    return data_reader


def chain(*readers):
    """
    Use the input data readers to create a chained data reader. The new created reader
    chains the outputs of input readers together as its output.

    **Note**:
        ``paddle.reader.chain`` is the alias of ``paddle.fluid.io.chain``, and
        ``paddle.fluid.io.chain`` is recommended to use.

    For example, if three input readers' outputs are as follows:
    [0, 0, 0],
    [10, 10, 10],
    [20, 20, 20].
    The chained reader will output:
    [[0, 0, 0], [10, 10, 10], [20, 20, 20]].

    Args:
        readers(list): input data readers.

    Returns:
        callable: the new chained data reader.

    Examples:
        ..  code-block:: python

            import paddle

            def reader_creator_3(start):
                def reader():
                    for i in range(start, start + 3):
                        yield [i, i, i]
                return reader

            c = paddle.reader.chain(reader_creator_3(0), reader_creator_3(10), reader_creator_3(20))
            for e in c():
                print(e)
            # Output:
            # [0, 0, 0]
            # [1, 1, 1]
            # [2, 2, 2]
            # [10, 10, 10]
            # [11, 11, 11]
            # [12, 12, 12]
            # [20, 20, 20]
            # [21, 21, 21]
            # [22, 22, 22]

    """

    def reader():
        rs = []
        for r in readers:
            rs.append(r())

        for e in itertools.chain(*rs):
            yield e

    return reader


class ComposeNotAligned(ValueError):
    pass


def compose(*readers, **kwargs):
    """
    Creates a data reader whose output is the combination of input readers.

    If input readers output following data entries:
    (1, 2)    3    (4, 5)
    The composed reader will output:
    (1, 2, 3, 4, 5)

    :param readers: readers that will be composed together.
    :param check_alignment: if True, will check if input readers are aligned
        correctly. If False, will not check alignment and trailing outputs
        will be discarded. Defaults to True.
    :type check_alignment: bool

    :return: the new data reader.

    :raises ComposeNotAligned: outputs of readers are not aligned.
        Will not raise when check_alignment is set to False.
    """
    check_alignment = kwargs.pop('check_alignment', True)

    def make_tuple(x):
        if isinstance(x, tuple):
            return x
        else:
            return (x, )

    def reader():
        rs = []
        for r in readers:
            rs.append(r())
        if not check_alignment:
            for outputs in zip(*rs):
                yield sum(list(map(make_tuple, outputs)), ())
        else:
            for outputs in zip_longest(*rs):
                for o in outputs:
                    if o is None:
                        # None will be not be present if compose is aligned
                        raise ComposeNotAligned(
                            "outputs of readers are not aligned.")
                yield sum(list(map(make_tuple, outputs)), ())

    return reader


def buffered(reader, size):
    """
    Creates a buffered data reader.

    The buffered data reader will read and save data entries into a
    buffer. Reading from the buffered data reader will proceed as long
    as the buffer is not empty.

    :param reader: the data reader to read from.
    :type reader: callable
    :param size: max buffer size.
    :type size: int

    :returns: the buffered data reader.
    """

    class EndSignal():
        pass

    end = EndSignal()

    def read_worker(r, q):
        for d in r:
            q.put(d)
        q.put(end)

    def data_reader():
        r = reader()
        q = Queue(maxsize=size)
        t = Thread(
            target=read_worker, args=(
                r,
                q, ))
        t.daemon = True
        t.start()
        e = q.get()
        while e != end:
            yield e
            e = q.get()

    return data_reader


def firstn(reader, n):
    """
    Limit the max number of samples that reader could return.

    :param reader: the data reader to read from.
    :type reader: callable
    :param n: the max number of samples that return.
    :type n: int
    :return: the decorated reader.
    :rtype: callable
    """

    # TODO(yuyang18): Check if just drop the reader, could clean the opened
    # resource or not?

    def firstn_reader():
        for i, item in enumerate(reader()):
            if i == n:
                break
            yield item

    return firstn_reader


class XmapEndSignal():
    pass


def xmap_readers(mapper, reader, process_num, buffer_size, order=False):
    """
    Use multi-threads to map samples from reader by a mapper defined by user.

    Args:
        mapper (callable): a function to map the data from reader.
        reader (callable): a data reader which yields the data. 
        process_num (int): thread number to handle original sample.
        buffer_size (int): size of the queue to read data in. 
        order (bool): whether to keep the data order from original reader. 
            Default False.

    Returns:
        callable: a decorated reader with data mapping. 
    """
    end = XmapEndSignal()

    # define a worker to read samples from reader to in_queue
    def read_worker(reader, in_queue):
        for i in reader():
            in_queue.put(i)
        in_queue.put(end)

    # define a worker to read samples from reader to in_queue with order flag
    def order_read_worker(reader, in_queue):
        in_order = 0
        for i in reader():
            in_queue.put((in_order, i))
            in_order += 1
        in_queue.put(end)

    # define a worker to handle samples from in_queue by mapper
    # and put mapped samples into out_queue
    def handle_worker(in_queue, out_queue, mapper):
        sample = in_queue.get()
        while not isinstance(sample, XmapEndSignal):
            r = mapper(sample)
            out_queue.put(r)
            sample = in_queue.get()
        in_queue.put(end)
        out_queue.put(end)

    # define a worker to handle samples from in_queue by mapper
    # and put mapped samples into out_queue by order
    def order_handle_worker(in_queue, out_queue, mapper, out_order):
        ins = in_queue.get()
        while not isinstance(ins, XmapEndSignal):
            order, sample = ins
            r = mapper(sample)
            while order != out_order[0]:
                pass
            out_queue.put(r)
            out_order[0] += 1
            ins = in_queue.get()
        in_queue.put(end)
        out_queue.put(end)

    def xreader():
        in_queue = Queue(buffer_size)
        out_queue = Queue(buffer_size)
        out_order = [0]
        # start a read worker in a thread
        target = order_read_worker if order else read_worker
        t = Thread(target=target, args=(reader, in_queue))
        t.daemon = True
        t.start()
        # start several handle_workers
        target = order_handle_worker if order else handle_worker
        args = (in_queue, out_queue, mapper, out_order) if order else (
            in_queue, out_queue, mapper)
        workers = []
        for i in range(process_num):
            worker = Thread(target=target, args=args)
            worker.daemon = True
            workers.append(worker)
        for w in workers:
            w.start()

        sample = out_queue.get()
        while not isinstance(sample, XmapEndSignal):
            yield sample
            sample = out_queue.get()
        finish = 1
        while finish < process_num:
            sample = out_queue.get()
            if isinstance(sample, XmapEndSignal):
                finish += 1
            else:
                yield sample

    return xreader


def multiprocess_reader(readers, use_pipe=True, queue_size=1000):
    """
    multiprocess_reader use python multi process to read data from readers
    and then use multiprocess.Queue or multiprocess.Pipe to merge all
    data. The process number is equal to the number of input readers, each
    process call one reader.

    Multiprocess.Queue require the rw access right to /dev/shm, some
    platform does not support.

    you need to create multiple readers first, these readers should be independent
    to each other so that each process can work independently.

    An example:

    .. code-block:: python

        reader0 = reader(["file01", "file02"])
        reader1 = reader(["file11", "file12"])
        reader1 = reader(["file21", "file22"])
        reader = multiprocess_reader([reader0, reader1, reader2],
            queue_size=100, use_pipe=False)
    """

    try:
        import ujson as json
    except Exception as e:
        sys.stderr.write("import ujson error: " + str(e) + " use json\n")
        import json

    assert type(readers) is list and len(readers) > 0

    def _read_into_queue(reader, queue):
        try:
            for sample in reader():
                if sample is None:
                    raise ValueError("sample has None")
                queue.put(sample)
            queue.put(None)
        except:
            queue.put("")
            six.reraise(*sys.exc_info())

    def queue_reader():
        queue = multiprocessing.Queue(queue_size)
        for reader in readers:
            p = multiprocessing.Process(
                target=_read_into_queue, args=(reader, queue))
            p.start()

        reader_num = len(readers)
        finish_num = 0
        while finish_num < reader_num:
            sample = queue.get()
            if sample is None:
                finish_num += 1
            elif sample == "":
                raise ValueError("multiprocess reader raises an exception")
            else:
                yield sample

    def _read_into_pipe(reader, conn):
        try:
            for sample in reader():
                if sample is None:
                    raise ValueError("sample has None!")
                conn.send(json.dumps(sample))
            conn.send(json.dumps(None))
            conn.close()
        except:
            conn.send(json.dumps(""))
            conn.close()
            six.reraise(*sys.exc_info())

    def pipe_reader():
        conns = []
        for reader in readers:
            parent_conn, child_conn = multiprocessing.Pipe()
            conns.append(parent_conn)
            p = multiprocessing.Process(
                target=_read_into_pipe, args=(reader, child_conn))
            p.start()

        reader_num = len(readers)
        finish_num = 0
        conn_to_remove = []
        while finish_num < reader_num:
            for conn in conn_to_remove:
                conns.remove(conn)
            conn_to_remove = []
            for conn in conns:
                sample = json.loads(conn.recv())
                if sample is None:
                    finish_num += 1
                    conn.close()
                    conn_to_remove.append(conn)
                elif sample == "":
                    conn.close()
                    conn_to_remove.append(conn)
                    raise ValueError("multiprocess reader raises an exception")
                else:
                    yield sample

    if use_pipe:
        return pipe_reader
    else:
        return queue_reader
