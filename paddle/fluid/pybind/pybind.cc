/* Copyright (c) 2016 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#include <Python.h>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>  // NOLINT // for call_once
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "paddle/fluid/framework/executor.h"
#include "paddle/fluid/framework/feed_fetch_method.h"
#include "paddle/fluid/framework/framework.pb.h"
#include "paddle/fluid/framework/garbage_collector.h"
#include "paddle/fluid/framework/ir/coalesce_grad_tensor_pass.h"
#include "paddle/fluid/framework/ir/pass_builder.h"
#include "paddle/fluid/framework/load_op_lib.h"
#include "paddle/fluid/framework/lod_rank_table.h"
#include "paddle/fluid/framework/lod_tensor.h"
#include "paddle/fluid/framework/lod_tensor_array.h"
#include "paddle/fluid/framework/op_compatible_info.h"
#include "paddle/fluid/framework/op_info.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/framework/parallel_executor.h"
#include "paddle/fluid/framework/prune.h"
#include "paddle/fluid/framework/reader.h"
#include "paddle/fluid/framework/save_load_util.h"
#include "paddle/fluid/framework/scope_pool.h"
#include "paddle/fluid/framework/selected_rows.h"
#include "paddle/fluid/framework/trainer.h"
#include "paddle/fluid/framework/version.h"
#include "paddle/fluid/imperative/layer.h"
#include "paddle/fluid/memory/allocation/allocator_strategy.h"
#include "paddle/fluid/operators/activation_op.h"
#include "paddle/fluid/operators/py_func_op.h"
#include "paddle/fluid/operators/reader/lod_tensor_blocking_queue.h"
#include "paddle/fluid/platform/cpu_helper.h"
#include "paddle/fluid/platform/cpu_info.h"
#include "paddle/fluid/platform/dynload/dynamic_loader.h"
#include "paddle/fluid/platform/enforce.h"
#include "paddle/fluid/platform/init.h"
#include "paddle/fluid/platform/place.h"
#include "paddle/fluid/platform/profiler.h"
#include "paddle/fluid/pybind/box_helper_py.h"
#include "paddle/fluid/pybind/const_value.h"
#include "paddle/fluid/pybind/data_set_py.h"
#include "paddle/fluid/pybind/exception.h"
#include "paddle/fluid/pybind/fleet_wrapper_py.h"
#include "paddle/fluid/pybind/imperative.h"
#include "paddle/fluid/pybind/inference_api.h"
#include "paddle/fluid/pybind/ir.h"

#ifndef _WIN32
#include "paddle/fluid/pybind/nccl_wrapper_py.h"
#endif
#include "paddle/fluid/framework/data_type.h"
#include "paddle/fluid/pybind/protobuf.h"
#include "paddle/fluid/pybind/pybind.h"  // NOLINT
#include "paddle/fluid/pybind/reader_py.h"
#include "paddle/fluid/pybind/tensor_py.h"
#include "paddle/fluid/string/to_string.h"
#ifdef PADDLE_WITH_CUDA
#ifndef _WIN32
#include "paddle/fluid/operators/nccl/nccl_gpu_common.h"
#endif
#include "paddle/fluid/platform/cuda_profiler.h"
#include "paddle/fluid/platform/gpu_info.h"
#endif

#ifdef PADDLE_WITH_DISTRIBUTE
#include "paddle/fluid/pybind/communicator_py.h"
#endif

#include "pybind11/stl.h"

DEFINE_bool(reader_queue_speed_test_mode, false,
            "If set true, the queue.pop will only get data from queue but not "
            "remove the data from queue for speed testing");
DECLARE_bool(use_mkldnn);
#ifdef PADDLE_WITH_NGRAPH
DECLARE_bool(use_ngraph);
#endif

// disable auto conversion to list in Python
PYBIND11_MAKE_OPAQUE(paddle::framework::LoDTensorArray);

namespace paddle {
namespace pybind {
bool IsCompiledWithCUDA() {
#ifndef PADDLE_WITH_CUDA
  return false;
#else
  return true;
#endif
}

bool IsCompiledWithMKLDNN() {
#ifndef PADDLE_WITH_MKLDNN
  return false;
#else
  return true;
#endif
}

bool IsCompiledWithNGRAPH() {
#ifndef PADDLE_WITH_NGRAPH
  return false;
#else
  return true;
#endif
}

bool IsCompiledWithBrpc() {
#ifndef PADDLE_WITH_DISTRIBUTE
  return false;
#endif

#ifdef PADDLE_WITH_GRPC
  return false;
#endif

  return true;
}

bool IsCompiledWithDIST() {
#ifdef PADDLE_WITH_DISTRIBUTE
  return true;
#else
  return false;
#endif
}

template <typename PlaceType1, typename PlaceType2>
static inline bool IsSamePlace(const PlaceType1 &p1, const PlaceType2 &p2) {
  return paddle::platform::Place(p1) == paddle::platform::Place(p2);
}

template <typename PlaceType>
static inline int PlaceIndex(const PlaceType &p) {
  return static_cast<int>(paddle::platform::Place(p).which());
}

static PyObject *GetPythonAttribute(PyObject *obj, const char *attr_name) {
  // NOTE(zjl): PyObject_GetAttrString would return nullptr when attr_name
  // is not inside obj, but it would also set the error flag of Python.
  // If the error flag is set in C++, C++ code would not raise Exception,
  // but Python would raise Exception once C++ call ends.
  // To avoid unexpected Exception raised in Python, we check whether
  // attribute exists before calling PyObject_GetAttrString.
  //
  // Caution: PyObject_GetAttrString would increase reference count of PyObject.
  // Developer should call Py_DECREF manually after the attribute is not used.
  if (PyObject_HasAttrString(obj, attr_name)) {
    return PyObject_GetAttrString(obj, attr_name);
  } else {
    return nullptr;
  }
}

template <typename T>
static T PyObjectCast(PyObject *obj) {
  try {
    return py::cast<T>(py::handle(obj));
  } catch (py::cast_error &) {
    PADDLE_THROW("Python object is not type of %s", typeid(T).name());
  }
}

using PyNameVarBaseMap = std::unordered_map<std::string, py::handle>;

static std::vector<std::shared_ptr<imperative::VarBase>> GetVarBaseList(
    const PyNameVarBaseMap &state_dict) {
  std::vector<std::shared_ptr<imperative::VarBase>> vec_res;
  vec_res.reserve(state_dict.size());

  for (auto &para : state_dict) {
    PyObject *py_obj = para.second.ptr();
    if (!py_obj || py_obj == Py_None) {
      PADDLE_THROW("Save parameter [%s] is None", para.first);
    }

    const char *kIVarField = "_ivar";
    PyObject *py_ivar = GetPythonAttribute(py_obj, kIVarField);
    PADDLE_ENFORCE_NOT_NULL(py_ivar, "Can not find  ivar in Variable");

    vec_res.emplace_back(
        PyObjectCast<std::shared_ptr<imperative::VarBase>>(py_ivar));
    Py_DECREF(py_ivar);
  }

  return vec_res;
}

static std::vector<std::string> inline GetNameList(
    const py::handle &py_handle) {
  std::vector<std::string> vec_res;

  PyObject *py_obj = py_handle.ptr();  // get underlying PyObject
  // Python None is not nullptr in C++!
  if (!py_obj || py_obj == Py_None) {
    PADDLE_THROW("Save parameter list is None");
  }

  if (PyList_Check(py_obj)) {
    size_t len = PyList_GET_SIZE(py_obj);

    vec_res.reserve(len);

    const char *kNameField = "name";

    for (size_t i = 0; i < len; ++i) {
      PyObject *py_name =
          PyObject_GetAttrString(PyList_GET_ITEM(py_obj, i), kNameField);
      PADDLE_ENFORCE_NOT_NULL(py_name);
      vec_res.emplace_back(PyObjectCast<std::string>(py_name));
      Py_DECREF(py_name);
    }
  } else {
    PADDLE_THROW("Set parameter should be a list");
  }

  return vec_res;
}

#ifdef PADDLE_WITH_AVX
PYBIND11_MODULE(core_avx, m) {
#else
PYBIND11_MODULE(core_noavx, m) {
#endif

  // Not used, just make sure cpu_info.cc is linked.
  paddle::platform::CpuTotalPhysicalMemory();

  paddle::memory::allocation::UseAllocatorStrategyGFlag();

  m.doc() = "C++ core of PaddlePaddle";

  // using framework in this function. Since it is inside a function, it will
  // not cause namespace pollution.
  using namespace paddle::framework;  // NOLINT

  BindException(&m);

  m.def("set_num_threads", &platform::SetNumThreads);

  m.def("_save_static_dict",
        [](const std::string &str_file_name, const py::handle &vec_var_list,
           const Scope &scope) {
          std::vector<std::string> vec_name_list = GetNameList(vec_var_list);
          SaveStaticNameListToDisk(str_file_name, vec_name_list, scope);
        });

  m.def("_load_static_dict",
        [](const std::string &str_file_name, const py::handle &vec_var_list,
           const Scope &scope) {
          std::vector<std::string> vec_name_list = GetNameList(vec_var_list);
          LoadStaticNameListFromDisk(str_file_name, vec_name_list, scope);
        });

  m.def("_save_dygraph_dict", [](const std::string &str_file_name,
                                 const PyNameVarBaseMap &state_dict) {
    auto vec_var_base_list = GetVarBaseList(state_dict);

    SaveDygraphVarBaseListToDisk(str_file_name, vec_var_base_list);
  });

  m.def("_load_dygraph_dict", [](const std::string &str_file_name) {
    auto load_tensor = LoadDygraphVarBaseListFromDisk(str_file_name);

    std::unordered_map<std::string, std::shared_ptr<imperative::VarBase>>
        map_output;

    for (size_t i = 0; i < load_tensor.size(); ++i) {
      map_output.emplace(load_tensor[i]->Name(), load_tensor[i]);
    }

    return map_output;
  });
  m.def("save_op_compatible_info", [](framework::ProgramDesc &desc) {
    framework::OpCompatibleMap op_compatible_map;
    op_compatible_map.InitOpCompatibleMap();
    return op_compatible_map.ConvertToProto(desc.OpCompatibleMap());
  });

  m.def(
      "_append_python_callable_object_and_return_id",
      [](py::object py_obj) -> size_t {
        return paddle::operators::AppendPythonCallableObjectAndReturnId(py_obj);
      });

  m.def("_get_use_default_grad_op_desc_maker_ops",
        [] { return OpInfoMap::Instance().GetUseDefaultGradOpDescMakerOps(); });

  // NOTE(zjl): ctest would load environment variables at the beginning even
  // though we have not `import paddle.fluid as fluid`. So we add this API
  // to enable eager deletion mode in unittest.
  m.def("_set_eager_deletion_mode", &paddle::framework::SetEagerDeletionMode);

  m.def("_set_fuse_parameter_group_size",
        &paddle::framework::ir::SetFuseParameterGroupsSize);
  m.def("_set_fuse_parameter_memory_size",
        &paddle::framework::ir::SetFuseParameterMemorySize);

  m.add_object("_cleanup",
               py::capsule([]() { ScopePool::Instance().Clear(); }));

  m.def("_set_paddle_lib_path", &paddle::platform::dynload::SetPaddleLibPath);

  BindImperative(&m);

  py::class_<Tensor>(m, "Tensor", py::buffer_protocol())
      .def("__array__", [](Tensor &self) { return TensorToPyArray(self); })
      .def("_is_initialized",
           [](const Tensor &self) { return self.IsInitialized(); })
      .def("_get_dims",
           [](const Tensor &self) { return vectorize(self.dims()); })
      .def("_set_dims",
           [](Tensor &self, const std::vector<int64_t> &dim) {
             self.Resize(make_ddim(dim));
           })
      .def("_set_layout",
           [](Tensor &self, const std::string &layout) {
             self.set_layout(StringToDataLayout(layout));
           })
      .def("_alloc_float",
           [](Tensor &self, paddle::platform::CUDAPlace &place) {
             self.mutable_data<float>(place);
           })
      .def("_alloc_float",
           [](Tensor &self, paddle::platform::CPUPlace &place) {
             self.mutable_data<float>(place);
           })
      .def("_alloc_double",
           [](Tensor &self, paddle::platform::CPUPlace &place) {
             self.mutable_data<double>(place);
           })
      .def("_alloc_int",
           [](Tensor &self, paddle::platform::CPUPlace &place) {
             self.mutable_data<int>(place);
           })
      .def("_alloc_int",
           [](Tensor &self, paddle::platform::CUDAPlace &place) {
             self.mutable_data<int>(place);
           })
      .def("_alloc_int",
           [](Tensor &self, paddle::platform::CUDAPinnedPlace &place) {
             self.mutable_data<int>(place);
           })
      .def("_alloc_float",
           [](Tensor &self, paddle::platform::CUDAPinnedPlace &place) {
             self.mutable_data<float>(place);
           })
      .def("_mutable_data",
           [](Tensor &self, paddle::platform::CPUPlace &place,
              paddle::framework::proto::VarType::Type type) {
             return reinterpret_cast<uintptr_t>(self.mutable_data(place, type));
           })
      .def("_mutable_data",
           [](Tensor &self, paddle::platform::CUDAPlace &place,
              paddle::framework::proto::VarType::Type type) {
             return reinterpret_cast<uintptr_t>(self.mutable_data(place, type));
           })
      .def("_mutable_data",
           [](Tensor &self, paddle::platform::CUDAPinnedPlace &place,
              paddle::framework::proto::VarType::Type type) {
             return reinterpret_cast<uintptr_t>(self.mutable_data(place, type));
           })
      .def("_clear", &Tensor::clear)
      .def("set", PyCPUTensorSetFromArray<float>)
      .def("set", PyCPUTensorSetFromArray<int>)
      .def("set", PyCPUTensorSetFromArray<double>)
      .def("set", PyCPUTensorSetFromArray<int64_t>)
      .def("set", PyCPUTensorSetFromArray<bool>)
      .def("set", PyCPUTensorSetFromArray<uint16_t>)
      .def("set", PyCPUTensorSetFromArray<uint8_t>)
      .def("set", PyCPUTensorSetFromArray<int8_t>)
#ifdef PADDLE_WITH_CUDA
      .def("set", PyCUDATensorSetFromArray<float>)
      .def("set", PyCUDATensorSetFromArray<int>)
      .def("set", PyCUDATensorSetFromArray<double>)
      .def("set", PyCUDATensorSetFromArray<int64_t>)
      .def("set", PyCUDATensorSetFromArray<bool>)
      .def("set", PyCUDATensorSetFromArray<uint16_t>)
      .def("set", PyCUDATensorSetFromArray<uint8_t>)
      .def("set", PyCUDATensorSetFromArray<int8_t>)
      .def("set", PyCUDAPinnedTensorSetFromArray<float>)
      .def("set", PyCUDAPinnedTensorSetFromArray<int>)
      .def("set", PyCUDAPinnedTensorSetFromArray<double>)
      .def("set", PyCUDAPinnedTensorSetFromArray<int64_t>)
      .def("set", PyCUDAPinnedTensorSetFromArray<bool>)
      .def("set", PyCUDAPinnedTensorSetFromArray<uint16_t>)
      .def("set", PyCUDAPinnedTensorSetFromArray<uint8_t>)
      .def("set", PyCUDAPinnedTensorSetFromArray<int8_t>)
#endif
      .def("shape", [](Tensor &self) { return vectorize(self.dims()); })
      .def("_set_float_element", TensorSetElement<float>)
      .def("_get_float_element", TensorGetElement<float>)
      .def("_set_double_element", TensorSetElement<double>)
      .def("_get_double_element", TensorGetElement<double>)
      .def("_place", [](Tensor &self) { return self.place(); })
      .def("_dtype", [](Tensor &self) { return self.type(); })
      .def("__getitem__", PySliceTensor, py::return_value_policy::reference)
      .def("__str__", [](const Tensor &self) {
        std::stringstream ostr;
        ostr << self;
        return ostr.str();
      });

  py::class_<LoDTensor, Tensor>(m, "LoDTensor", R"DOC(
    LoDTensor is a Tensor with optional LoD information.

    np.array(lod_tensor) can convert LoDTensor to numpy array.
    lod_tensor.lod() can retrieve the LoD information.

    LoD is short for Level of Details and is usually used for varied sequence
    length. You can skip the following comment if you don't need optional LoD.

    For example, a LoDTensor X can look like the example below. It contains
    2 sequences. The first has length 2 and the second has length 3, as
    described by x.lod.

    The first tensor dimension 5=2+3 is calculated from LoD if it's available.
    It means the total number of sequence element. In X, each element has 2
    columns, hence [5, 2].

    x.lod  = [[2, 3]]

    x.data = [[1, 2], [3, 4], [5, 6], [7, 8], [9, 10]]

    x.shape = [5, 2]

    LoD can have multiple levels (for example, a paragraph can have multiple
    sentences and a sentence can have multiple words). In the following
    LodTensor Y, the lod_level is 2. It means there are 2 sequence, the
    first sequence length is 2 (has 2 sub-sequences), the second one's
    length is 1. The first sequence's 2 sub-sequences have length 2 and 2,
    respectively. And the second sequence's 1 sub-sequence has length 3.

    y.lod = [[2 1], [2 2 3]]

    y.shape = [2+2+3, ...]

    Examples:
        .. code-block:: python

          import paddle.fluid as fluid

          t = fluid.LoDTensor()

  Note:
      In above description, LoD is length-based. In Paddle internal
      implementation, lod is offset-based. Hence, internally,
      y.lod is represented as [[0, 2, 3], [0, 2, 4, 7]] (length-based
      equivlent would be [[2-0, 3-2], [2-0, 4-2, 7-4]]).

      Sometimes LoD is called recursive_sequence_length to be more
      self-explanatory. In this case, it must be length-based. Due to history
      reasons. when LoD is called lod in public API, it might be offset-based.
      Users should be careful about it.
        )DOC")
      .def("__array__", [](Tensor &self) { return TensorToPyArray(self); })
      .def("__init__",
           [](LoDTensor &instance, const std::vector<std::vector<size_t>>
                                       &recursive_sequence_lengths) {
             LoD new_lod;
             new_lod.reserve(recursive_sequence_lengths.size());
             std::copy(recursive_sequence_lengths.begin(),
                       recursive_sequence_lengths.end(),
                       std::back_inserter(new_lod));
             LoD new_offset_lod = ConvertToOffsetBasedLoD(new_lod);
             PADDLE_ENFORCE_EQ(
                 CheckLoD(new_offset_lod, -1), true,
                 "the provided recursive_sequence_lengths info is invalid");
             new (&instance) LoDTensor(new_offset_lod);
           })
      .def("__init__", [](LoDTensor &instance) { new (&instance) LoDTensor(); })
      // We implement offset based LOD in C++ while we use length based with
      // Python API. So we changed set_lod to set_recursive_sequence_lengths
      // to
      // avoid misuse.
      // The discussion is here:
      // https://github.com/PaddlePaddle/Paddle/issues/10855
      .def("set_lod",
           [](LoDTensor &self, const std::vector<std::vector<size_t>> &lod) {
             // the input lod is offset-based level-of-detail info
             LoD new_lod;
             new_lod.reserve(lod.size());
             std::copy(lod.begin(), lod.end(), std::back_inserter(new_lod));
             PADDLE_ENFORCE_EQ(
                 CheckLoD(new_lod, vectorize(self.dims()).front()), true,
                 "the provided lod info is invalid");
             self.set_lod(new_lod);
           },
           py::arg("lod"), R"DOC(
           Set LoD of the LoDTensor.

           Args:
               lod (List[List[int]]): the lod to be set.

           Examples:
               .. code-block:: python

                 import paddle.fluid as fluid
                 import numpy as np

                 t = fluid.LoDTensor()
                 t.set(np.ndarray([5, 30]), fluid.CPUPlace())
                 t.set_lod([[0, 2, 5]])
           )DOC")
      .def("set_recursive_sequence_lengths",
           [](LoDTensor &self, const std::vector<std::vector<size_t>>
                                   &recursive_sequence_lengths) {
             // the input recursive_sequence_lengths is length-based
             // level-of-detail info
             LoD new_lod;
             new_lod.reserve(recursive_sequence_lengths.size());
             std::copy(recursive_sequence_lengths.begin(),
                       recursive_sequence_lengths.end(),
                       std::back_inserter(new_lod));
             LoD new_offset_lod = ConvertToOffsetBasedLoD(new_lod);
             PADDLE_ENFORCE_EQ(
                 CheckLoD(new_offset_lod, vectorize(self.dims()).front()), true,
                 "the provided recursive_sequence_lengths info is invalid");
             self.set_lod(new_offset_lod);
           },
           py::arg("recursive_sequence_lengths"), R"DOC(
           Set LoD of the LoDTensor according to recursive sequence length.

           For example, if recursive_sequence_lengths=[[2, 3]], meaning that
           there are two sequences with length 2 and 3 respectively, the
           corresponding lod would be [[0, 2, 2+3]], i.e, [[0, 2, 5]].

           Args:
                recursive_sequence_lengths (List[List[int]]): sequence lengths.

           Examples:
               .. code-block:: python

                 import paddle.fluid as fluid
                 import numpy as np

                 t = fluid.LoDTensor()
                 t.set(np.ndarray([5, 30]), fluid.CPUPlace())
                 t.set_recursive_sequence_lengths([[2, 3]])
           )DOC")
      .def("lod",
           [](LoDTensor &self) -> std::vector<std::vector<size_t>> {
             // output the offset-based lod info
             LoD lod = self.lod();
             std::vector<std::vector<size_t>> new_lod;
             new_lod.reserve(lod.size());
             std::copy(lod.begin(), lod.end(), std::back_inserter(new_lod));
             return new_lod;
           },
           R"DOC(
           Return the LoD of the LoDTensor.

           Returns:
               out (List[List[int]]): the lod of the LoDTensor.

           Examples:
               .. code-block:: python

                 import paddle.fluid as fluid
                 import numpy as np

                 t = fluid.LoDTensor()
                 t.set(np.ndarray([5, 30]), fluid.CPUPlace())
                 t.set_lod([[0, 2, 5]])
                 print(t.lod()) # [[0, 2, 5]]
           )DOC")
      // Set above comments of set_lod.
      .def("recursive_sequence_lengths",
           [](LoDTensor &self) -> std::vector<std::vector<size_t>> {
             // output the length-based lod info
             LoD lod = ConvertToLengthBasedLoD(self.lod());
             std::vector<std::vector<size_t>> new_lod;
             new_lod.reserve(lod.size());
             std::copy(lod.begin(), lod.end(), std::back_inserter(new_lod));
             return new_lod;
           },
           R"DOC(
           Return the sequence length of the LoDTensor corresponding to LoD.

           Returns:
               out (List[List[int]): the sequence lengths.

           Examples:
               .. code-block:: python

                 import paddle.fluid as fluid
                 import numpy as np

                 t = fluid.LoDTensor()
                 t.set(np.ndarray([5, 30]), fluid.CPUPlace())
                 t.set_recursive_sequence_lengths([[2, 3]])
                 print(t.recursive_sequence_lengths()) # [[2, 3]]
           )DOC")
      .def("has_valid_recursive_sequence_lengths",
           [](LoDTensor &self) -> bool {
             // Check that the lod info is valid and match the outermost
             // dimension of the LoDTensor data
             return CheckLoD(self.lod(), vectorize(self.dims()).front());
           },
           R"DOC(
           Check whether the lod of the LoDTensor is valid.

           Returns:
               out (bool): whether the lod is valid.

           Examples:
               .. code-block:: python

                 import paddle.fluid as fluid
                 import numpy as np

                 t = fluid.LoDTensor()
                 t.set(np.ndarray([5, 30]), fluid.CPUPlace())
                 t.set_recursive_sequence_lengths([[2, 3]])
                 print(t.has_valid_recursive_sequence_lengths()) # True
           )DOC")
      .def("__getitem__", PySliceTensor, py::return_value_policy::reference,
           R"DOC(
           Slice the original Tensor, and remove the LoD information.

           Returns:
               out (Tensor): new Tensor(NOT LoDTensor).
           )DOC")
      .def("__str__",
           [](const LoDTensor &self) {
             std::stringstream ostr;
             ostr << self;
             return ostr.str();
           })
      .def("_copy", [](const LoDTensor &self, const platform::Place &place) {
        // follow fetch_op's inplementation
        LoDTensor dst;
        if (self.IsInitialized() && self.numel() > 0) {
          TensorCopySync(self, place, &dst);
        } else {
          // Not copy, if the src tensor is empty.
          dst.clear();
          dst.Resize({0});
        }
        dst.set_lod(self.lod());
        return dst;
      });

  py::class_<SelectedRows>(m, "SelectedRows")
      .def("__init__",
           [](SelectedRows &instance) { new (&instance) SelectedRows(); })
      .def("__init__",
           [](SelectedRows &instance, const std::vector<int64_t> rows,
              const int64_t &height) {
             new (&instance) SelectedRows(rows, height);
           })
      .def("get_tensor",
           [](SelectedRows &self) { return self.mutable_value(); },
           py::return_value_policy::reference)
      .def("numel",
           [](SelectedRows &self) -> int64_t { return self.value().numel(); })
      .def("set_height", &SelectedRows::set_height)
      .def("height", &SelectedRows::height)
      .def("set_rows",
           [](SelectedRows &self, std::vector<int64_t> rows) {
#ifndef PADDLE_WITH_CUDA
             self.set_rows(rows);
#else
        Vector<int64_t> new_rows(rows);
        self.set_rows(new_rows);
#endif
           })
      .def("sync_index", [](SelectedRows &instance) { instance.SyncIndex(); })
      .def("rows", [](SelectedRows &self) {
        auto rows = self.rows();
        std::vector<int64_t> new_rows;
        new_rows.reserve(rows.size());
        std::copy(rows.begin(), rows.end(), std::back_inserter(new_rows));
        return new_rows;
      });

  py::class_<Variable>(m, "Variable", R"DOC(Variable Class.

All parameter, weight, gradient are variables in Paddle.
)DOC")
      .def(py::init<>())
      .def("is_int", [](const Variable &var) { return var.IsType<int>(); })
      .def("set_int",
           [](Variable &var, int val) -> void { *var.GetMutable<int>() = val; })
      .def("get_int", [](const Variable &var) -> int { return var.Get<int>(); })
      .def("is_float", [](const Variable &var) { return var.IsType<float>(); })
      .def("set_float",
           [](Variable &var, float val) -> void {
             *var.GetMutable<float>() = val;
           })
      .def("get_float",
           [](const Variable &var) -> float { return var.Get<float>(); })
      .def("get_tensor",
           [](Variable &self) -> LoDTensor * {
             return self.GetMutable<LoDTensor>();
           },
           py::return_value_policy::reference)
      .def("get_lod_rank_table",
           [](Variable &self) { return self.GetMutable<LoDRankTable>(); },
           py::return_value_policy::reference)
      .def("get_selected_rows",
           [](Variable &self) -> SelectedRows * {
             return self.GetMutable<SelectedRows>();
           },
           py::return_value_policy::reference)
      .def("get_lod_tensor_array",
           [](Variable &self) { return self.GetMutable<LoDTensorArray>(); },
           py::return_value_policy::reference)
#if (defined(PADDLE_WITH_CUDA) && !defined(_WIN32))
      .def("get_communicator",
           [](Variable &self) -> platform::Communicator * {
             return self.GetMutable<platform::Communicator>();
           },
           py::return_value_policy::reference)
#endif
      .def("get_reader",
           [](Variable &self) -> framework::ReaderHolder * {
             PADDLE_ENFORCE_EQ(self.IsType<framework::ReaderHolder>(), true);
             return self.GetMutable<framework::ReaderHolder>();
           },
           py::return_value_policy::reference);

  BindReader(&m);

  using LoDTensorBlockingQueue =
      ::paddle::operators::reader::LoDTensorBlockingQueue;
  using LoDTensorBlockingQueueHolder =
      ::paddle::operators::reader::LoDTensorBlockingQueueHolder;

  py::class_<LoDTensorBlockingQueue, std::shared_ptr<LoDTensorBlockingQueue>>(
      m, "LoDTensorBlockingQueue", "")
      .def("push",
           [](LoDTensorBlockingQueue &self,
              const std::vector<framework::LoDTensor> &lod_tensor_vec) {
             pybind11::gil_scoped_release release;
             return self.Push(lod_tensor_vec);
           })
      .def("size", &LoDTensorBlockingQueue::Size)
      .def("capacity", &LoDTensorBlockingQueue::Cap)
      .def("close", &LoDTensorBlockingQueue::Close)
      .def("is_closed", &LoDTensorBlockingQueue::IsClosed);

  m.def("init_lod_tensor_blocking_queue",
        [](Variable &var,
           size_t capacity) -> std::shared_ptr<LoDTensorBlockingQueue> {
          VLOG(1) << "init_lod_tensor_blocking_queue";
          auto *holder = var.GetMutable<LoDTensorBlockingQueueHolder>();
          holder->InitOnce(capacity, FLAGS_reader_queue_speed_test_mode);
          return holder->GetQueue();
        },
        py::return_value_policy::copy);

  py::class_<Scope>(m, "_Scope", R"DOC(
    Scope is an association of a name to Variable. All variables belong to Scope.

    Variables in a parent scope can be retrieved from local scope.

    You need to specify a scope to run a Net, i.e., `exe.Run(&scope)`.
    One net can run in different scopes and update different variable in the
    scope.

    You can create var in a scope and get it from the scope.

    Examples:
        .. code-block:: python

          import paddle.fluid as fluid
          # create tensor from a scope and set value to it.
          param = scope.var('Param').get_tensor()
          param_array = np.full((height, row_numel), 5.0).astype("float32")
          param.set(param_array, place)

        )DOC")
      .def("_remove_from_pool",
           [](Scope &self) { ScopePool::Instance().Remove(&self); })
      .def("var",
           [](Scope &self, const std::string &name) -> Variable * {
             return self.Var(name);
           },
           py::arg("name"),
           R"DOC(
           Find or create variable named :code:`name` in the current scope.

           If the variable named :code:`name` does not exist in the
           current scope, the variable would be created. Otherwise,
           return the existing variable.

           Args:
               name (str): the variable name.

           Returns:
               out (core.Variable): the found or created variable.
           )DOC",
           py::return_value_policy::reference)
      .def("find_var", &Scope::FindVar, py::arg("name"),
           R"DOC(
           Find variable named :code:`name` in the current scope or
           its parent scope. Return None if not found.

           Args:
               name (str): the variable name.

           Returns:
               out (core.Variable|None): the found variable or None.
           )DOC",
           py::return_value_policy::reference)
      .def("new_scope", [](Scope &self) -> Scope * { return &self.NewScope(); },
           R"DOC(
           Create a new sub-scope of the current scope.

           Returns:
               out (core._Scope): the created sub-scope.
           )DOC",
           py::return_value_policy::reference)
      .def("drop_kids", &Scope::DropKids,
           R"DOC(
           Delete all sub-scopes of the current scope.
           )DOC")
      .def("_kids", &Scope::kids);

  m.def("Scope",
        []() -> Scope * {
          auto *s = new Scope();
          ScopePool::Instance().Insert(std::unique_ptr<Scope>(s));
          return s;
        },
        R"DOC(
        Create a new scope.

        Returns:
            out (core._Scope): the created scope.
        )DOC",
        py::return_value_policy::reference);

  //! @note: Be careful! PyBind will return std::string as an unicode, not
  //! Python str. If you want a str object, you should cast them in Python.
  m.def("get_all_op_protos", []() -> std::vector<py::bytes> {
    std::vector<py::bytes> ret_values;
    for (auto &iter : OpInfoMap::Instance().map()) {
      auto &info = iter.second;
      if (info.HasOpProtoAndChecker()) {
        std::string str;
        PADDLE_ENFORCE_EQ(
            info.Proto().SerializeToString(&str), true,
            "Serialize OpProto Error. This could be a bug of Paddle.");
        ret_values.emplace_back(str);
      }
    }
    return ret_values;
  });
  m.def(
      "get_grad_op_desc", [](const OpDesc &op_desc,
                             const std::unordered_set<std::string> &no_grad_set,
                             const std::vector<BlockDesc *> &grad_sub_block) {
        std::unordered_map<std::string, std::string> grad_to_var;
        std::vector<std::unique_ptr<OpDesc>> grad_op_descs =
            framework::OpInfoMap::Instance()
                .Get(op_desc.Type())
                .GradOpMaker()(op_desc, no_grad_set, &grad_to_var,
                               grad_sub_block);
        std::vector<OpDesc *> grad_op_desc_ptrs(grad_op_descs.size());
        std::transform(grad_op_descs.begin(), grad_op_descs.end(),
                       grad_op_desc_ptrs.begin(),
                       [](std::unique_ptr<OpDesc> &p) { return p.release(); });
        return std::make_pair(grad_op_desc_ptrs, grad_to_var);
      });
  m.def("has_grad_op_maker", [](const std::string op_type) {
    return framework::OpInfoMap::Instance().Get(op_type).HasGradOpMaker();
  });
  m.def("has_infer_inplace", [](const std::string op_type) {
    return framework::OpInfoMap::Instance().Get(op_type).HasInferInplace();
  });
  m.def("get_flags_use_mkldnn", []() { return FLAGS_use_mkldnn; });
#ifdef PADDLE_WITH_NGRAPH
  m.def("get_flags_use_ngraph", []() { return FLAGS_use_ngraph; });
#endif

  m.def("prune", [](const ProgramDesc &origin,
                    const std::set<std::string> &feeded_var_names,
                    const std::vector<std::array<size_t, 2>> &targets) {
    ProgramDesc prog_with_targets(origin);

    for (const auto &t : targets) {
      prog_with_targets.MutableBlock(t[0])->Op(t[1])->SetIsTarget(true);
    }
    proto::ProgramDesc pruned_desc;
    Prune(*prog_with_targets.Proto(), feeded_var_names, &pruned_desc);
    return new ProgramDesc(pruned_desc);
  });
  m.def("prune_backward", [](const framework::ProgramDesc &program) {
    return PruneBackward(program);
  });
  m.def("empty_var_name",
        []() { return std::string(framework::kEmptyVarName); });
  m.def("grad_var_suffix",
        []() { return std::string(framework::kGradVarSuffix); });
  m.def_submodule(
       "var_names",
       "The module will return special predefined variable name in Paddle")
      .def("empty", []() { return kEmptyVarName; })
      .def("temp", []() { return kTempVarName; });
  // clang-format off
  py::class_<paddle::platform::DeviceContext>(m, "DeviceContext")
      .def_static("create",
                  [](paddle::platform::CPUPlace& place)
                      -> paddle::platform::DeviceContext* {
                    return new paddle::platform::CPUDeviceContext();
                  })
      .def_static("create",
                  [](paddle::platform::CUDAPlace& place)
                      -> paddle::platform::DeviceContext* {
#ifndef PADDLE_WITH_CUDA
                    PADDLE_THROW("CUDAPlace is not supported in CPU device.");
#else
                    return new paddle::platform::CUDADeviceContext(place);
#endif
                  })
          .def_static("create",
                [](paddle::platform::CUDAPinnedPlace& place)
                        -> paddle::platform::DeviceContext* {
#ifndef PADDLE_WITH_CUDA
                  PADDLE_THROW(
                        "CUDAPinnedPlace is not supported in CPU device.");
#else
                  return new paddle::platform::CUDAPinnedDeviceContext(place);
#endif
                });;
// clang-format on
#if (defined(PADDLE_WITH_CUDA) && !defined(_WIN32))
  py::class_<platform::Communicator>(m, "Communicator").def(py::init<>());
#endif
  py::class_<platform::CUDAPlace>(m, "CUDAPlace", R"DOC(
    **Note**:
        For multi-card tasks, please use `FLAGS_selected_gpus` environment variable to set the visible GPU device.
        The next version will fix the problem with `CUDA_VISIBLE_DEVICES` environment variable.

    CUDAPlace is a descriptor of a device.
    It represents a GPU device allocated or to be allocated with Tensor or LoDTensor.
    Each CUDAPlace has a dev_id to indicate the graphics card ID represented by the current CUDAPlace,
    staring from 0.
    The memory of CUDAPlace with different dev_id is not accessible.
    Numbering here refers to the logical ID of the visible graphics card, not the actual ID of the graphics card.
    You can set visible GPU devices by setting the `CUDA_VISIBLE_DEVICES` environment variable.
    When the program starts, visible GPU devices will be numbered from 0.
    If `CUDA_VISIBLE_DEVICES` is not set, all devices are visible by default,
    and the logical ID is the same as the actual ID.

    Parameters:
        id (int): GPU device ID.

    Examples:
        .. code-block:: python

          import paddle.fluid as fluid
          gpu_place = fluid.CUDAPlace(0)

        )DOC")
      .def("__init__",
           [](platform::CUDAPlace &self, int dev_id) {
#ifdef PADDLE_WITH_CUDA
             if (UNLIKELY(dev_id < 0)) {
               LOG(ERROR) << string::Sprintf(
                   "Invalid CUDAPlace(%d), device id must be 0 or "
                   "positive integer",
                   dev_id);
               std::exit(-1);
             }

             if (UNLIKELY(dev_id >= platform::GetCUDADeviceCount())) {
               if (platform::GetCUDADeviceCount() == 0) {
                 LOG(ERROR) << "Cannot use GPU because there is no GPU "
                               "detected on your "
                               "machine.";
                 std::exit(-1);
               } else {
                 LOG(ERROR) << string::Sprintf(
                     "Invalid CUDAPlace(%d), must inside [0, %d), because GPU "
                     "number on your machine is %d",
                     dev_id, platform::GetCUDADeviceCount(),
                     platform::GetCUDADeviceCount());
                 std::exit(-1);
               }
             }

             new (&self) platform::CUDAPlace(dev_id);
#else
             LOG(ERROR) << string::Sprintf(
                 "Cannot use GPU because you have installed CPU version "
                 "PaddlePaddle.\n"
                 "If you want to use GPU, please try to install GPU version "
                 "PaddlePaddle by: pip install paddlepaddle-gpu\n"
                 "If you only have CPU, please change CUDAPlace(%d) to be "
                 "CPUPlace().\n",
                 dev_id);
             std::exit(-1);
#endif
           })
      .def("_type", &PlaceIndex<platform::CUDAPlace>)
      .def("_equals", &IsSamePlace<platform::CUDAPlace, platform::Place>)
      .def("_equals", &IsSamePlace<platform::CUDAPlace, platform::CUDAPlace>)
      .def("_equals", &IsSamePlace<platform::CUDAPlace, platform::CPUPlace>)
      .def("_equals",
           &IsSamePlace<platform::CUDAPlace, platform::CUDAPinnedPlace>)
      .def("__str__", string::to_string<const platform::CUDAPlace &>);

  py::class_<paddle::platform::CPUPlace>(m, "CPUPlace", R"DOC(
    CPUPlace is a descriptor of a device.
    It represents a CPU device allocated or to be allocated with Tensor or LoDTensor.

    Examples:
        .. code-block:: python

          import paddle.fluid as fluid
          cpu_place = fluid.CPUPlace()to be allocated

        )DOC")
      .def(py::init<>())
      .def("_type", &PlaceIndex<platform::CPUPlace>)
      .def("_equals", &IsSamePlace<platform::CPUPlace, platform::Place>)
      .def("_equals", &IsSamePlace<platform::CPUPlace, platform::CUDAPlace>)
      .def("_equals", &IsSamePlace<platform::CPUPlace, platform::CPUPlace>)
      .def("_equals",
           &IsSamePlace<platform::CPUPlace, platform::CUDAPinnedPlace>)
      .def("__str__", string::to_string<const platform::CPUPlace &>);

  py::class_<paddle::platform::CUDAPinnedPlace>(m, "CUDAPinnedPlace", R"DOC(
    CUDAPinnedPlace is a descriptor of a device.
    It refers to the page locked memory allocated by the CUDA function `cudaHostAlloc()` in the host memory.
    The host operating system will not paging and exchanging the memory.
    It can be accessed through direct memory access technology to speed up the copy of data between the host and GPU.
    For more information on CUDA data transfer and `pinned memory`,
    please refer to `official document <https://docs.nvidia.com/cuda/cuda-c-best-practices-guide/index.html#pinned-memory>`_ .

    Examples:
        .. code-block:: python

          import paddle.fluid as fluid
          place = fluid.CUDAPinnedPlace()

        )DOC")
      .def("__init__",
           [](platform::CUDAPinnedPlace &self) {
#ifndef PADDLE_WITH_CUDA
             PADDLE_THROW("Cannot use CUDAPinnedPlace in CPU only version");
#endif
             new (&self) platform::CUDAPinnedPlace();
           })
      .def("_type", &PlaceIndex<platform::CUDAPinnedPlace>)
      .def("_equals", &IsSamePlace<platform::CUDAPinnedPlace, platform::Place>)
      .def("_equals",
           &IsSamePlace<platform::CUDAPinnedPlace, platform::CUDAPlace>)
      .def("_equals",
           &IsSamePlace<platform::CUDAPinnedPlace, platform::CPUPlace>)
      .def("_equals",
           &IsSamePlace<platform::CUDAPinnedPlace, platform::CUDAPinnedPlace>)
      .def("__str__", string::to_string<const platform::CUDAPinnedPlace &>);

  py::class_<platform::Place>(m, "Place")
      .def(py::init<>())
      .def("_type", &PlaceIndex<platform::Place>)
      .def("_equals", &IsSamePlace<platform::Place, platform::Place>)
      .def("_equals", &IsSamePlace<platform::Place, platform::CUDAPlace>)
      .def("_equals", &IsSamePlace<platform::Place, platform::CPUPlace>)
      .def("_equals", &IsSamePlace<platform::Place, platform::CUDAPinnedPlace>)
      .def("is_gpu_place",
           [](platform::Place &self) { return platform::is_gpu_place(self); })
      .def("is_cpu_place",
           [](platform::Place &self) { return platform::is_cpu_place(self); })
      .def("is_cuda_pinned_place",
           [](platform::Place &self) {
             return platform::is_cuda_pinned_place(self);
           })
      .def("gpu_device_id",
           [](platform::Place &self) {
             return boost::get<platform::CUDAPlace>(self).device;
           })
      .def("set_place", [](platform::Place &self,
                           const platform::Place &other) { self = other; })
      .def("set_place",
           [](platform::Place &self, const platform::CPUPlace &cpu_place) {
             self = cpu_place;
           })
      .def("set_place",
           [](platform::Place &self, const platform::CUDAPlace &gpu_place) {
             self = gpu_place;
           })
      .def("set_place", [](platform::Place &self,
                           const platform::CUDAPinnedPlace &cuda_pinned_place) {
        self = cuda_pinned_place;
      });

  py::class_<OperatorBase>(m, "Operator")
      .def_static(
          "create",
          [](py::bytes protobin) {
            proto::OpDesc desc;
            PADDLE_ENFORCE_EQ(desc.ParsePartialFromString(protobin), true,
                              "Cannot parse user input to OpDesc");
            PADDLE_ENFORCE_EQ(desc.IsInitialized(), true,
                              "User OpDesc is not initialized, reason %s",
                              desc.InitializationErrorString());
            return OpRegistry::CreateOp(desc);
          })
      .def("run",
           [](OperatorBase &self, const Scope &scope,
              const platform::CPUPlace &place) { self.Run(scope, place); })
      .def("run",
           [](OperatorBase &self, const Scope &scope,
              const platform::CUDAPlace &place) { self.Run(scope, place); })
      .def("run",
           [](OperatorBase &self, const Scope &scope,
              const platform::CUDAPinnedPlace &place) {
             self.Run(scope, place);
           })
      .def("type",
           [](const OperatorBase &op) -> std::string { return op.Type(); })
      .def("outputs",
           [](const OperatorBase &op)
               -> std::map<std::string, std::vector<std::string>> {
                 return op.Outputs();
               })
      .def("output_vars",
           [](const OperatorBase &op) { return op.OutputVars(true); })
      .def("inputs", [](const OperatorBase &op) { return op.Inputs(); })
      .def("input_vars", [](const OperatorBase &op) { return op.InputVars(); })
      .def("__str__", &OperatorBase::DebugString)
      .def("no_intermediate_outputs",
           [](const OperatorBase &op) { return op.OutputVars(false); })
      .def("support_gpu", &OperatorBase::SupportGPU);

  py::class_<framework::ExecutorPrepareContext>(m, "ExecutorPrepareContext")
      .def(py::init<const ProgramDesc &, size_t>());

  py::class_<framework::TrainerBase, std::shared_ptr<framework::TrainerBase>>(
      m, "TrainerBase")
      .def("get_worker_scope",
           [](TrainerBase &self, int thread_id) -> Scope * {
             return self.GetWorkerScope(thread_id);
           },
           py::return_value_policy::reference)
      .def("finalize", &TrainerBase::Finalize);

  py::class_<framework::Executor>(m, "Executor")
      .def(py::init<const platform::Place &>())
      .def("close", &Executor::Close)
      .def("run_from_dataset", &Executor::RunFromDataset,
           py::call_guard<py::gil_scoped_release>())
      .def("init_for_dataset",
           [](Executor &self, const ProgramDesc &prog,
              const std::string &trainer_desc, Scope *scope,
              Dataset *dataset) -> std::shared_ptr<TrainerBase> {
             return self.InitForDataset(prog, trainer_desc, scope, dataset);
           })
      .def("run_from_dataset",
           [](Executor &self, std::shared_ptr<TrainerBase> trainer) {
             pybind11::gil_scoped_release release;
             self.RunFromDataset(trainer);
           })
      .def("run_prepared_ctx",
           [](Executor &self, ExecutorPrepareContext *ctx, Scope *scope,
              std::map<std::string, const LoDTensor *> *feed_targets,
              std::map<std::string, LoDTensor *> *fetch_targets,
              bool create_local_scope = true, bool create_vars = true,
              const std::string &feed_holder_name = "feed",
              const std::string &fetch_holder_name = "fetch") {
             pybind11::gil_scoped_release release;
             self.RunPreparedContext(ctx, scope, feed_targets, fetch_targets,
                                     create_local_scope, create_vars,
                                     feed_holder_name, fetch_holder_name);
           })
      .def("run_cached_prepared_ctx",
           [](Executor &self, ExecutorPrepareContext *ctx, Scope *scope,
              bool create_local_scope = true, bool create_vars = true,
              bool keep_kids = false) {
             pybind11::gil_scoped_release release;
             self.RunPreparedContext(ctx, scope, create_local_scope,
                                     create_vars, keep_kids);
           })
      .def("prepare_ctx_cache", &Executor::PrepareCtxCache,
           py::call_guard<py::gil_scoped_release>())
      .def("create_variables", &Executor::CreateVariables,
           py::call_guard<py::gil_scoped_release>())
      .def("run", [](Executor &self, const ProgramDesc &prog, Scope *scope,
                     int block_id, bool create_local_scope, bool create_vars,
                     const std::vector<std::string> &fetch_vars) {
        pybind11::gil_scoped_release release;
        self.Run(prog, scope, block_id, create_local_scope, create_vars,
                 fetch_vars);
      });

  m.def("init_gflags", framework::InitGflags);
  m.def("init_glog", framework::InitGLOG);
  m.def("init_dgc", framework::InitDGC);
  m.def("load_op_library", framework::LoadOpLib);
  m.def("init_devices",
        [](bool init_p2p) { framework::InitDevices(init_p2p); });

  m.def("is_compiled_with_ngraph", IsCompiledWithNGRAPH);
  m.def("is_compiled_with_cuda", IsCompiledWithCUDA);
  m.def("is_compiled_with_mkldnn", IsCompiledWithMKLDNN);
  m.def("is_compiled_with_brpc", IsCompiledWithBrpc);
  m.def("is_compiled_with_dist", IsCompiledWithDIST);
#ifdef PADDLE_WITH_CUDA
  m.def("is_float16_supported", [](const platform::CUDAPlace &place) -> bool {
    // Only GPUs with Compute Capability >= 53 support float16
    return platform::GetCUDAComputeCapability(place.device) >= 53;
  });
#endif

  m.def("set_feed_variable", framework::SetFeedVariable);
  m.def("get_fetch_variable", framework::GetFetchVariable);
  m.def("get_variable_tensor", framework::GetVariableTensor);

  m.def("_is_program_version_supported", IsProgramVersionSupported);

  BindProgramDesc(&m);
  BindBlockDesc(&m);
  BindVarDsec(&m);
  BindOpDesc(&m);
  BindConstValue(&m);

  py::class_<framework::LoDRankTable>(m, "LodRankTable")
      .def("items", [](framework::LoDRankTable &table) {
        std::vector<std::pair<size_t, size_t>> res;
        for (auto &item : table.items()) {
          res.push_back({item.index, item.length});
        }
        return res;
      });

  py::class_<LoDTensorArray>(m, "LoDTensorArray", R"DOC(
    LoDTensorArray is array of LoDTensor, it supports operator[], len() and for-loop iteration.

    Examples:
        .. code-block:: python

          import paddle.fluid as fluid

          arr = fluid.LoDTensorArray()
)DOC")
      .def("__init__",
           [](LoDTensorArray &instance) { new (&instance) LoDTensorArray(); })
      .def("__getitem__",
           [](LoDTensorArray &self, size_t i) { return &self.at(i); },
           py::return_value_policy::reference)
      .def("__len__", [](LoDTensorArray &self) { return self.size(); })
      .def("__setitem__",
           [](LoDTensorArray &self, size_t i, const LoDTensor &t) {
             PADDLE_ENFORCE_LT(i, self.size());
             self[i].ShareDataWith(t);
             self[i].set_lod(t.lod());
           })
      .def("append",
           [](LoDTensorArray &self, const LoDTensor &t) {
             self.emplace_back();
             self.back().ShareDataWith(t);
             self.back().set_lod(t.lod());
           },
           py::arg("tensor"), R"DOC(
             Append a LoDensor to LoDTensorArray.
              
             Args:
                   tensor (LoDTensor): The LoDTensor to be appended.

             Returns:
                   None.

             Examples:
                 .. code-block:: python

                   import paddle.fluid as fluid
                   import numpy as np

                   arr = fluid.LoDTensorArray()
                   t = fluid.LoDTensor()
                   t.set(np.ndarray([5, 30]), fluid.CPUPlace())
                   arr.append(t)
           )DOC")
      .def("_move_to_list",
           [](LoDTensorArray &self) -> py::list {
             py::list res(self.size());
             for (size_t i = 0; i < self.size(); ++i) {
               res[i] = py::cast(std::move(self[i]));
             }
             self.clear();
             return res;
           },
           py::return_value_policy::take_ownership);

  m.def("op_support_gpu", OpSupportGPU);
#ifdef PADDLE_WITH_CUDA
  m.def("get_cuda_device_count", platform::GetCUDADeviceCount);

#ifndef _WIN32
  m.def("nvprof_init", platform::CudaProfilerInit);
  m.def("nvprof_start", platform::CudaProfilerStart);
  m.def("nvprof_stop", platform::CudaProfilerStop);
#endif
#endif

  py::enum_<platform::ProfilerState>(m, "ProfilerState", py::arithmetic())
      .value("kDisabled", platform::ProfilerState::kDisabled)
      .value("kCPU", platform::ProfilerState::kCPU)
      .value("kCUDA", platform::ProfilerState::kCUDA)
      .value("kAll", platform::ProfilerState::kAll)
      .export_values();

  py::enum_<platform::EventSortingKey>(m, "EventSortingKey", py::arithmetic())
      .value("kDefault", platform::EventSortingKey::kDefault)
      .value("kCalls", platform::EventSortingKey::kCalls)
      .value("kTotal", platform::EventSortingKey::kTotal)
      .value("kMin", platform::EventSortingKey::kMin)
      .value("kMax", platform::EventSortingKey::kMax)
      .value("kAve", platform::EventSortingKey::kAve)
      .export_values();

  m.def("enable_profiler", platform::EnableProfiler);
  m.def("disable_profiler", platform::DisableProfiler);
  m.def("is_profiler_enabled", platform::IsProfileEnabled);
  m.def("reset_profiler", platform::ResetProfiler);
  m.def("get_pass", [](const std::string &pass_type) {
    auto pass = framework::ir::PassRegistry::Instance().Get(pass_type);
    return std::shared_ptr<framework::ir::Pass>(std::move(pass));
  });

  m.def("size_of_dtype", framework::SizeOfType);

  using VarQuantScale =
      std::unordered_map<std::string, std::pair<bool, LoDTensor>>;

  py::class_<ir::Pass, std::shared_ptr<ir::Pass>> pass(m, "Pass");
  pass.def(py::init())
      .def("has", &ir::Pass::Has)
      .def("set_not_owned",
           [](ir::Pass &self, const std::string &attr_name, ProgramDesc &attr) {
             self.SetNotOwned<ProgramDesc>(attr_name, &attr);
           })
      .def(
          "set",
          [](ir::Pass &self, const std::string &name, const std::string &attr) {
            self.Set<std::string>(name, new std::string(attr));
          })
      .def("set", [](ir::Pass &self, const std::string &name,
                     int val) { self.Set<const int>(name, new int(val)); })
      .def("set",
           [](ir::Pass &self, const std::string &name,
              std::unordered_set<std::string> set) {
             self.Set(name, new std::unordered_set<std::string>(set));
           })
      .def("set",
           [](ir::Pass &self, const std::string &name,
              std::unordered_set<int> set) {
             self.Set(name, new std::unordered_set<int>(set));
           })
      .def("set",
           [](ir::Pass &self, const std::string &name, VarQuantScale scales) {
             self.Set(name, new VarQuantScale(scales));
           })
      .def("type", &ir::Pass::Type)
      .def("apply", [](ir::Pass &self, std::shared_ptr<ir::Graph> graph) {
        self.Apply(graph.get());
      });

  py::class_<ir::PassBuilder, std::shared_ptr<ir::PassBuilder>> pb(
      m, "PassBuilder");
  pb.def(py::init())
      .def("append_pass",
           [](ir::PassBuilder &self,
              const std::string &pass_type) -> std::shared_ptr<ir::Pass> {
             return self.AppendPass(pass_type);
           })
      .def("all_passes", [](ir::PassBuilder &self) { return self.AllPasses(); })
      .def("insert_pass",
           [](ir::PassBuilder &self, size_t idx, const std::string &pass_type) {
             return self.InsertPass(idx, pass_type);
           })
      .def("remove_pass",
           [](ir::PassBuilder &self, size_t idx) { self.RemovePass(idx); });

  // -- python binds for parallel executor.

  py::class_<ParallelExecutor> pe(m, "ParallelExecutor");
  py::class_<ExecutionStrategy> exec_strategy(pe, "ExecutionStrategy", R"DOC(
    ExecutionStrategy allows the user to more preciously control how to run
    the program in ParallelExecutor by setting the property.

    Examples:
        .. code-block:: python

          import paddle.fluid as fluid
          x = fluid.layers.data(name='x', shape=[13], dtype='float32')
          y = fluid.layers.data(name='y', shape=[1], dtype='float32')
          y_predict = fluid.layers.fc(input=x, size=1, act=None)

          cost = fluid.layers.square_error_cost(input=y_predict, label=y)
          avg_loss = fluid.layers.mean(cost)

          sgd_optimizer = fluid.optimizer.SGD(learning_rate=0.001)
          sgd_optimizer.minimize(avg_loss)

          exec_strategy = fluid.ExecutionStrategy()
          exec_strategy.num_threads = 4

          train_exe = fluid.ParallelExecutor(use_cuda=False,
                                             loss_name=avg_loss.name,
                                             exec_strategy=exec_strategy)

        )DOC");

  exec_strategy.def(py::init())
      .def_property(
          "num_threads",
          [](const ExecutionStrategy &self) { return self.num_threads_; },
          [](ExecutionStrategy &self, size_t num_threads) {
            self.num_threads_ = num_threads;
          },
          R"DOC(The type is INT, num_threads represents the size of thread pool that
            used to run the operators of the current program in ParallelExecutor.
            If :math:`num\_threads=1`, all the operators will execute one by one,
            but the order maybe difference between iterations.
            If it is not set, it will be set in ParallelExecutor according to the
            device type and device count, for GPU, :math:`num\_threads=device\_count*4`, for CPU,
            :math:`num\_threads=CPU\_NUM*4`, the explanation of:math:`CPU\_NUM` is in ParallelExecutor.
            if it is not set, ParallelExecutor will get the cpu count by calling
            `multiprocessing.cpu_count()`. Default 0.)DOC")
      .def_property(
          "use_cuda",
          [](const ExecutionStrategy &self) { return self.use_cuda_; },
          [](ExecutionStrategy &self, bool use_cuda) {
            self.use_cuda_ = use_cuda;
          })  // FIXME(chengduo): Doesn't add doc for 'use_cuda', use_cuda may
      // make user confuse, because ParallelExecutor has a parameter named
      // 'use_cuda' too, in current implementation, ParallelExecutor's
      // 'use_cuda' will rewrite ExecutionStrategy's 'use_cuda'.
      .def_property(
          "allow_op_delay",
          [](const ExecutionStrategy &self) { return self.allow_op_delay_; },
          [](ExecutionStrategy &self, bool allow_op_delay) {
            self.allow_op_delay_ = allow_op_delay;
          },
          R"DOC(The type is BOOL, allow_op_delay represents whether to delay the
                communication operators to run, it may make the execution faster.
                Note that this option is invalid now, and it will be removed in
                next version. Default False.)DOC")
      .def_property(
          "num_iteration_per_drop_scope",
          [](const ExecutionStrategy &self) {
            return self.num_iteration_per_drop_scope_;
          },
          [](ExecutionStrategy &self, size_t num_iteration_per_drop_scope) {
            self.num_iteration_per_drop_scope_ = num_iteration_per_drop_scope;
          },
          R"DOC(The type is INT, num_iteration_per_drop_scope indicates how
                many iterations to clean up the temp variables which
                is generated during execution. It may make the execution faster,
                because the temp variable's shape maybe the same between two iterations.
                Default 1.

                NOTES:
                    1. If you fetch data when calling the 'run', the ParallelExecutor
                       will clean up the temp variables at the end of the current iteration.
                    2. In some NLP model, it may cause the GPU memory is insufficient,
                       in this case, you should reduce `num_iteration_per_drop_scope`.
              )DOC")
      .def_property(
          "num_iteration_per_run",
          [](const ExecutionStrategy &self) {
            return self.num_iteration_per_run_;
          },
          [](ExecutionStrategy &self, size_t num_iteration_per_run) {
            self.num_iteration_per_run_ = num_iteration_per_run;
          },
          R"DOC(This config that how many iteration the executor will run when
                user call pe.run() in python
              )DOC")
      .def_property("_dry_run",
                    [](const ExecutionStrategy &self) { return self.dry_run_; },
                    [](ExecutionStrategy &self, bool dry_run) {
                      self.dry_run_ = dry_run;
                    });

  exec_strategy.def_property(
      "use_experimental_executor",
      [](const ExecutionStrategy &self) {
        return self.type_ == ExecutionStrategy::kExperimental;
      },
      [](ExecutionStrategy &self, bool experimental) {
        self.type_ = experimental ? ExecutionStrategy::kExperimental
                                  : ExecutionStrategy::kDefault;
      });

  py::class_<BuildStrategy> build_strategy(pe, "BuildStrategy", R"DOC(
    BuildStrategy allows the user to more preciously control how to
    build the SSA Graph in ParallelExecutor by setting the property.

    Examples:
        .. code-block:: python

            import paddle.fluid as fluid
            build_strategy = fluid.BuildStrategy()
            build_strategy.reduce_strategy = fluid.BuildStrategy.ReduceStrategy.Reduce
)DOC");

  py::enum_<BuildStrategy::ReduceStrategy>(build_strategy, "ReduceStrategy")
      .value("Reduce", BuildStrategy::ReduceStrategy::kReduce)
      .value("AllReduce", BuildStrategy::ReduceStrategy::kAllReduce);
  py::enum_<BuildStrategy::GradientScaleStrategy>(build_strategy,
                                                  "GradientScaleStrategy")
      .value("CoeffNumDevice",
             BuildStrategy::GradientScaleStrategy::kCoeffNumDevice)
      .value("One", BuildStrategy::GradientScaleStrategy::kOne)
      .value("Customized", BuildStrategy::GradientScaleStrategy::kCustomized);

  build_strategy.def(py::init())
      .def_property(
          "reduce_strategy",
          [](const BuildStrategy &self) { return self.reduce_; },
          [](BuildStrategy &self, BuildStrategy::ReduceStrategy strategy) {
            PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                              "BuildStrategy is finlaized.");
            self.reduce_ = strategy;
          },
          R"DOC(The type is fluid.BuildStrategy.ReduceStrategy, there are two reduce
                strategies in ParallelExecutor, AllReduce and Reduce. If you want
                that all the parameters' optimization are done on all devices independently,
                you should choose AllReduce; if you choose Reduce, all the parameters'
                optimization will be evenly distributed to different devices, and then
                broadcast the optimized parameter to other devices.
                Default 'AllReduce'.

                Examples:
                    .. code-block:: python

                        import paddle.fluid as fluid
                        build_strategy = fluid.BuildStrategy()
                        build_strategy.reduce_strategy = fluid.BuildStrategy.ReduceStrategy.Reduce
                  )DOC")
      .def_property(
          "gradient_scale_strategy",
          [](const BuildStrategy &self) { return self.gradient_scale_; },
          [](BuildStrategy &self,
             BuildStrategy::GradientScaleStrategy strategy) {
            PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                              "BuildStrategy is finalized.");
            self.gradient_scale_ = strategy;
          },
          R"DOC(The type is fluid.BuildStrategy.GradientScaleStrategy, there are three
                ways of defining :math:`loss@grad` in ParallelExecutor, CoeffNumDevice,
                One and Customized. By default, ParallelExecutor sets the :math:`loss@grad`
                according to the number of devices. If you want to customize :math:`loss@grad`,
                you can choose Customized. Default 'CoeffNumDevice'.

                Examples:
                    .. code-block:: python

                        import paddle.fluid as fluid
                        import paddle.fluid.compiler as compiler
                        import numpy
                        import os

                        use_cuda = True
                        place = fluid.CUDAPlace(0) if use_cuda else fluid.CPUPlace()
                        exe = fluid.Executor(place)

                        # NOTE: If you use CPU to run the program, you need
                        # to specify the CPU_NUM, otherwise, fluid will use
                        # all the number of the logic core as the CPU_NUM,
                        # in that case, the batch size of the input should be
                        # greater than CPU_NUM, if not, the process will be
                        # failed by an exception.
                        if not use_cuda:
                            os.environ['CPU_NUM'] = str(2)
                            places = fluid.cpu_places()
                        else:
                            places = places = fluid.cuda_places()

                        data = fluid.layers.data(name='X', shape=[1], dtype='float32')
                        hidden = fluid.layers.fc(input=data, size=10)
                        loss = fluid.layers.mean(hidden)
                        fluid.optimizer.SGD(learning_rate=0.01).minimize(loss)

                        fluid.default_startup_program().random_seed=1
                        exe.run(fluid.default_startup_program())

                        build_strategy = fluid.BuildStrategy()
                        build_strategy.gradient_scale_strategy = \
                                 fluid.BuildStrategy.GradientScaleStrategy.Customized
                        compiled_prog = compiler.CompiledProgram(
                                 fluid.default_main_program()).with_data_parallel(
                                          loss_name=loss.name, build_strategy=build_strategy,
                                          places = places)

                        dev_count =  len(places)
                        x = numpy.random.random(size=(10, 1)).astype('float32')
                        loss_grad = numpy.ones((dev_count)).astype("float32") * 0.01
                        loss_grad_name = loss.name+"@GRAD"
                        loss_data = exe.run(compiled_prog,
                                             feed={"X": x, loss_grad_name : loss_grad},
                                             fetch_list=[loss.name, loss_grad_name])
                   )DOC")
      .def_property(
          "debug_graphviz_path",
          [](const BuildStrategy &self) { return self.debug_graphviz_path_; },
          [](BuildStrategy &self, const std::string &path) {
            PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                              "BuildStrategy is finlaized.");
            self.debug_graphviz_path_ = path;
          },
          R"DOC(The type is STR, debug_graphviz_path indicates the path that
                writing the SSA Graph to file in the form of graphviz.
                It is useful for debugging. Default ""

                Examples:
                    .. code-block:: python

                        import paddle.fluid as fluid
                        build_strategy = fluid.BuildStrategy()
                        build_strategy.debug_graphviz_path = "./graph"

                    )DOC")
      .def_property(
          "enable_sequential_execution",
          [](const BuildStrategy &self) {
            return self.enable_sequential_execution_;
          },
          [](BuildStrategy &self, bool b) {
            PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                              "BuildStrategy is finlaized.");
            self.enable_sequential_execution_ = b;
          },
          R"DOC(The type is BOOL. If set True, the execution order of ops would
                be the same as what is in the program. Default False.

                Examples:
                    .. code-block:: python

                        import paddle.fluid as fluid
                        build_strategy = fluid.BuildStrategy()
                        build_strategy.enable_sequential_execution = True
          )DOC")
      .def_property(
          "remove_unnecessary_lock",
          [](const BuildStrategy &self) {
            return self.remove_unnecessary_lock_;
          },
          [](BuildStrategy &self, bool b) {
            PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                              "BuildStrategy is finlaized.");
            self.remove_unnecessary_lock_ = b;
          },
          R"DOC(The type is BOOL. If set True, some locks in GPU ops would be
                released and ParallelExecutor would run faster. Default True.

                Examples:
                    .. code-block:: python

                        import paddle.fluid as fluid
                        build_strategy = fluid.BuildStrategy()
                        build_strategy.remove_unnecessary_lock = True
          )DOC")
      .def_property(
          "num_trainers",
          [](const BuildStrategy &self) { return self.num_trainers_; },
          [](BuildStrategy &self, int num_trainers) {
#ifdef WIN32
            PADDLE_THROW("Windows has NO support to distribute mode.");
#endif
            self.num_trainers_ = num_trainers;
          })
      .def_property(
          "trainers_endpoints",
          [](const BuildStrategy &self) { return self.trainers_endpoints_; },
          [](BuildStrategy &self,
             const std::vector<std::string> &trainers_endpoints) {
            self.trainers_endpoints_ = trainers_endpoints;
          })
      .def_property("trainer_id",
                    [](const BuildStrategy &self) { return self.trainer_id_; },
                    [](BuildStrategy &self, int trainer_id) {
                      self.trainer_id_ = trainer_id;
                    })
      .def_property(
          "nccl_comm_num",
          [](const BuildStrategy &self) { return self.nccl_comm_num_; },
          [](BuildStrategy &self, int nccl_comm_num) {
            self.nccl_comm_num_ = nccl_comm_num;
          })
      .def_property("use_hierarchical_allreduce",
                    [](const BuildStrategy &self) {
                      return self.use_hierarchical_allreduce_;
                    },
                    [](BuildStrategy &self, bool use) {
                      self.use_hierarchical_allreduce_ = use;
                    })
      .def_property("hierarchical_allreduce_inter_nranks",
                    [](const BuildStrategy &self) {
                      return self.hierarchical_allreduce_inter_nranks_;
                    },
                    [](BuildStrategy &self, int nranks) {
                      self.hierarchical_allreduce_inter_nranks_ = nranks;
                    })

      .def_property(
          "fuse_elewise_add_act_ops",
          [](const BuildStrategy &self) {
            return self.fuse_elewise_add_act_ops_;
          },
          [](BuildStrategy &self, bool b) {
            PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                              "BuildStrategy is finlaized.");
            self.fuse_elewise_add_act_ops_ = b;
          },
          R"DOC(The type is BOOL, fuse_elewise_add_act_ops indicate whether
                to fuse elementwise_add_op and activation_op,
                it may make the execution faster. Default False

                Examples:
                    .. code-block:: python

                        import paddle.fluid as fluid
                        build_strategy = fluid.BuildStrategy()
                        build_strategy.fuse_elewise_add_act_ops = True
                     )DOC")
      .def_property(
          "fuse_relu_depthwise_conv",
          [](const BuildStrategy &self) {
            return self.fuse_relu_depthwise_conv_;
          },
          [](BuildStrategy &self, bool b) {
            PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                              "BuildStrategy is finlaized.");
            self.fuse_relu_depthwise_conv_ = b;
          },
          R"DOC(The type is BOOL, fuse_relu_depthwise_conv indicate whether
                to fuse relu and depthwise_conv2d,
                it will save GPU memory and may make the execution faster.
                This options is only available in GPU devices.
                Default False.

                Examples:
                    .. code-block:: python

                        import paddle.fluid as fluid
                        build_strategy = fluid.BuildStrategy()
                        build_strategy.fuse_relu_depthwise_conv = True
          )DOC")
      .def_property("fuse_broadcast_ops",
                    [](const BuildStrategy &self) {
                      return self.fuse_broadcast_ops_ == true ||
                             self.fuse_broadcast_ops_ == boost::none;
                    },
                    [](BuildStrategy &self, bool b) {
                      PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                                        "BuildStrategy is finlaized.");
                      self.fuse_broadcast_ops_ = b;
                    },
                    R"DOC(The type is BOOL, fuse_broadcast_op indicates whether
                      to fuse the broadcast ops. Note that, in Reduce mode,
                      fusing broadcast ops may make the program faster. Because
                      fusing broadcast OP equals delaying the execution of all
                      broadcast Ops, in this case, all nccl streams are used only
                      for NCCLReduce operations for a period of time. Default False.)DOC")
      .def_property("fuse_all_optimizer_ops",
                    [](const BuildStrategy &self) {
                      return self.fuse_all_optimizer_ops_ == true ||
                             self.fuse_all_optimizer_ops_ == boost::none;
                    },
                    [](BuildStrategy &self, bool b) {
                      PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                                        "BuildStrategy is finlaized.");
                      self.fuse_all_optimizer_ops_ = b;
                    })
      .def_property(
          "sync_batch_norm",
          [](const BuildStrategy &self) { return self.sync_batch_norm_; },
          [](BuildStrategy &self, bool b) {
            PADDLE_ENFORCE_EQ(!self.IsFinalized(), true,
                              "BuildStrategy is finlaized.");
            self.sync_batch_norm_ = b;
          },
          R"DOC(The type is BOOL, sync_batch_norm indicates whether to use
                synchronous batch normalization which synchronizes the mean
                and variance through multi-devices in training phase.

                Current implementation doesn't support FP16 training and CPU.
                And only synchronous on one machine, not all machines.

                Default False

                Examples:
                    .. code-block:: python

                        import paddle.fluid as fluid
                        build_strategy = fluid.BuildStrategy()
                        build_strategy.sync_batch_norm = True
                )DOC")
      .def_property(
          "memory_optimize",
          [](const BuildStrategy &self) -> py::object {
            if (self.memory_optimize_) {
              return py::cast(self.memory_optimize_.get());
            } else {
              return py::cast(nullptr);
            }
          },
          [](BuildStrategy &self, const py::handle &value) {
            auto *py_obj = value.ptr();
            if (py_obj == nullptr || py_obj == Py_None) {
              self.memory_optimize_ = boost::none;
            } else if (PyBool_Check(py_obj)) {
              self.memory_optimize_ = (py_obj == Py_True);
            } else {
              PADDLE_THROW(
                  "BuildStrategy.memory_optimize must be None, False or "
                  "True");
            }
          },
          R"DOC(The type is BOOL or None, memory opitimize aims to save total memory
                consumption, set to True to enable it.

                Default None. None means framework would choose to use or not use 
                this strategy automatically. Currently, None means that it is 
                enabled when GC is disabled, and disabled when GC is enabled. 
                True means enabling and False means disabling. Default None.)DOC")
      .def_property(
          "is_distribution",
          [](const BuildStrategy &self) { return self.is_distribution_; },
          [](BuildStrategy &self, bool b) {
#ifdef WIN32
            if (b) {
              PADDLE_THROW("Windows has NO support to distribute mode.");
            }
#else
            self.is_distribution_ = b;
#endif
          })
      .def_property("async_mode",
                    [](const BuildStrategy &self) { return self.async_mode_; },
                    [](BuildStrategy &self, bool b) { self.async_mode_ = b; })
      .def_property(
          "enable_inplace",
          [](const BuildStrategy &self) { return self.enable_inplace_; },
          [](BuildStrategy &self, bool b) { self.enable_inplace_ = b; })
      .def_property(
          "fuse_all_reduce_ops",
          [](const BuildStrategy &self) {
            return self.fuse_all_reduce_ops_ == true ||
                   self.fuse_all_reduce_ops_ == boost::none;
          },
          [](BuildStrategy &self, bool b) { self.fuse_all_reduce_ops_ = b; })
      .def_property("enable_backward_optimizer_op_deps",
                    [](const BuildStrategy &self) {
                      return self.enable_backward_optimizer_op_deps_;
                    },
                    [](BuildStrategy &self, bool b) {
                      self.enable_backward_optimizer_op_deps_ = b;
                    })
      .def_property(
          "cache_runtime_context",
          [](const BuildStrategy &self) { return self.cache_runtime_context_; },
          [](BuildStrategy &self, bool b) { self.cache_runtime_context_ = b; })
      .def_property(
          "mkldnn_enabled_op_types",
          [](const BuildStrategy &self) {
            return self.mkldnn_enabled_op_types_;
          },
          [](BuildStrategy &self,
             const std::unordered_set<std::string> &mkldnn_enabled_op_types) {
            self.mkldnn_enabled_op_types_ = mkldnn_enabled_op_types;
          })
      .def("_finalize_strategy_and_create_passes",
           [](BuildStrategy &self) -> std::shared_ptr<ir::PassBuilder> {
             return self.CreatePassesFromStrategy(true);
           },
           R"DOC(Allow user to customized passes. Normally model-specific
                optimization passes should be defined in this way. BuildStrategy
                cannot be updated after being finalized.)DOC");

  pe.def(py::init<const std::vector<platform::Place> &,
                  const std::vector<std::string> &, const std::string &,
                  Scope *, std::vector<Scope *> &, const ExecutionStrategy &,
                  const BuildStrategy &, ir::Graph *>())
      // NOTE: even we return a vec<Scope*>* to Python use reference policy.
      // We still cannot get local_scope from this vector, since the element
      // of vec<Scope*> will be freed by Python GC. We can only return Scope*
      // one by one and mark them as reference.
      .def("local_scopes",
           [](ParallelExecutor &self) -> std::vector<Scope *> * {
             return &self.GetLocalScopes();
           },
           py::return_value_policy::reference)
      .def("drop_local_exe_scopes", &ParallelExecutor::DropLocalExeScopes)
      .def("_need_create_local_exe_scopes",
           &ParallelExecutor::NeedCreateLocalExeScope)
      .def("feed_tensors_into_local_scopes",
           &ParallelExecutor::FeedTensorsIntoLocalScopes)
      .def("feed_and_split_tensor_into_local_scopes",
           &ParallelExecutor::FeedAndSplitTensorIntoLocalScopes)
      .def("run", [](ParallelExecutor &self,
                     const std::vector<std::string> &fetch_tensors) {
        pybind11::gil_scoped_release release;
        return self.Run(fetch_tensors);
      });

  BindFleetWrapper(&m);
  BindBoxHelper(&m);
#ifndef _WIN32
  BindNCCLWrapper(&m);
#endif
  BindGraph(&m);
  BindNode(&m);
  BindInferenceApi(&m);
  BindDataset(&m);
#ifdef PADDLE_WITH_DISTRIBUTE
  BindCommunicator(&m);
#endif
}
}  // namespace pybind
}  // namespace paddle
