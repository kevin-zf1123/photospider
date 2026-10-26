#pragma once

// Included within the frequency namespace after the certified line kernels.
namespace staged_internal {
inline constexpr const char* contract = "frq-ball-stages-v1";
inline bool eligible(const Frequency& s, const OperationLowering& q) {
  return false && s.dims.size() == 2 && s.grid.ya == 0 && s.grid.xa == 1 &&
         (s.kind == Kind::Fft || s.kind == Kind::Ifft) &&
         s.dims[0] == s.dims[1] && s.dims[0] >= 1024 &&
         power2(s.dims[0]) && q.backend == physical::Backend::Cpu;
}
inline std::uint64_t scratch(std::uint64_t n, std::uint64_t count) {
  return sizeof(Math) + 128 + (n * count + n) * sizeof(Complex) + n * 8;
}
struct Owner {
  std::shared_ptr<Frequency> state;
  Kernel reader;
  std::string symbol;
  std::uint64_t start = 0, count = 0, original_reads = 0;
  ps_prepared_kernel_v9 inner{};
  ~Owner() {
    if (inner.release)
      inner.release(inner.context);
  }
};
inline const std::uint8_t* address(const ps_kernel_const_view_v8& v,
                                  std::uint64_t y, std::uint64_t x = 0) {
  return static_cast<const std::uint8_t*>(v.allocation) + v.byte_offset +
         (y-v.layout->origin[0])*v.layout->strides[0] +
         (v.layout->rank == 3 ? (x-v.layout->origin[1])*v.layout->strides[1] : 0);
}
inline std::uint8_t* address(const ps_kernel_mutable_view_v8& v,
                            std::uint64_t y, std::uint64_t x = 0) {
  return static_cast<std::uint8_t*>(v.allocation) + v.byte_offset +
         (y-v.layout->origin[0])*v.layout->strides[0] +
         (v.layout->rank == 3 ? (x-v.layout->origin[1])*v.layout->strides[1] : 0);
}
inline void invoke(const ps_kernel_invocation_v9* invocation,
                    ps_kernel_status_v8* status) {
  try {
    FloatingEnvironment environment;
    require(environment.active(), "FRQ staged floating environment");
    const auto& owner =
        *static_cast<const Owner*>(invocation->views.prepared_context);
    const auto& s = *owner.state;
    const auto& inv = invocation->views;
    const auto n = s.dims[0];
    const Work work{inv, s.work_limit, 0, invocation->work};
    const Fuel fuel{work};
    fuel.poll();
    auto* data = reinterpret_cast<Complex*>(
        static_cast<std::uint8_t*>(inv.scratch[0].allocation)+sizeof(Math));
    if (owner.symbol == "frq-ball-final") {
      const TransformBlock copy{work, 4*n*n};
      for (unsigned i = owner.original_reads; i < inv.input_count; ++i) {
        const auto& v = inv.inputs[i];
        for (std::uint64_t y = v.layout->origin[0];
             y < v.layout->origin[0]+v.layout->shape[0]; ++y)
          for (std::uint64_t x = v.layout->origin[1];
               x < v.layout->origin[1]+v.layout->shape[1]; ++x) {
            copy.poll(4);
            Complex value;
            std::memcpy(&value, address(v,y,x), sizeof(value));
            new (data+y*n+x) Complex{value};
          }
      }
      auto mapped = *invocation;
      mapped.views.input_count = owner.original_reads;
      mapped.views.prepared_context = owner.inner.context;
      mapped.views.parameter_bytes = 0;
      owner.inner.invoke_cpu(&mapped,status);
      return;
    }
    generation::MathArena arena(inv.scratch[0].allocation,work);
    arena->exact.bind(fuel);
    arena->d.consume = core_internal::WorkConsumer(fuel);
    auto* roots = data + n*owner.count;
    auto* indices = reinterpret_cast<std::uint64_t*>(roots+n);
    if (owner.symbol == "frq-ball-constants") {
      roots = data;
      indices = reinterpret_cast<std::uint64_t*>(roots+n);
      root_table(*arena,roots,n,n/2,s.inverse(),fuel,s.tables.get());
      const auto* cached = permutation(n,fuel,s.tables.get());
      const TransformBlock copy{work,6*n};
      for (unsigned i = 0; i < inv.output_count; ++i) {
        const auto& v = inv.outputs[i];
        for (std::uint64_t j = v.layout->origin[0];
             j < v.layout->origin[0]+v.layout->shape[0]; ++j) {
          copy.poll(4);
          if (v.layout->rank == 2)
            std::memcpy(address(v,j),roots+j,sizeof(Complex));
          else
            std::memcpy(address(v,j),cached+j,8);
        }
      }
    } else {
      const TransformBlock constants{work,6*n};
      for (unsigned i = 0; i < inv.input_count; ++i) {
        const auto& v=inv.inputs[i];
        if (v.port != 2 && v.port != 3)
          continue;
        for (std::uint64_t j = v.layout->origin[0];
             j < v.layout->origin[0]+v.layout->shape[0]; ++j) {
          constants.poll(4);
          if (v.port == 2)
            std::memcpy(roots+j,address(v,j),sizeof(Complex));
          else
            std::memcpy(indices+j,address(v,j),8);
        }
      }
      const bool rows = owner.symbol == "frq-ball-rows";
      const TransformBlock load{work,16*n*owner.count};
      if (rows) {
        const Reader reader{owner.reader,inv};
        for (std::uint64_t y = owner.start; y < owner.start+owner.count; ++y)
          for (std::uint64_t x = 0; x < n; ++x) {
            load.poll(16);
            Coordinate at{};at[0]=y;at[1]=x;
            new (data+(y-owner.start)*n+x)
                Complex{Ball::point(reader.value(0,0,at)),
                         Ball::point(reader.value(1,0,at))};
          }
      } else {
        for (unsigned i=0;i<inv.input_count;++i) {
          const auto& v=inv.inputs[i];
          if (v.port != 4)
            continue;
          for (std::uint64_t y=v.layout->origin[0];
               y<v.layout->origin[0]+v.layout->shape[0];++y)
            for (std::uint64_t x=v.layout->origin[1];
                 x<v.layout->origin[1]+v.layout->shape[1];++x) {
              load.poll(16);
              Complex value;std::memcpy(&value,address(v,y,x),sizeof(value));
              new (data+(x-owner.start)*n+y) Complex{value};
            }
        }
      }
      for (std::uint64_t line=0;line<owner.count;++line)
        fft_line(data+line*n,n,1,roots,n,fuel,nullptr,indices);
      const TransformBlock store{work,4*n*owner.count};
      for (unsigned i=0;i<inv.output_count;++i) {
        const auto& v=inv.outputs[i];
        for (std::uint64_t y=v.layout->origin[0];
             y<v.layout->origin[0]+v.layout->shape[0];++y)
          for (std::uint64_t x=v.layout->origin[1];
               x<v.layout->origin[1]+v.layout->shape[1];++x) {
            store.poll(4);
            const auto at=rows?(y-owner.start)*n+x:(x-owner.start)*n+y;
            std::memcpy(address(v,y,x),data+at,sizeof(Complex));
          }
      }
    }
    check(arena->exact.status());
    fuel.flush();
    *status={};
  } catch (const Status& failure) {
    *status=kernel_failure(failure.code,failure.message.c_str(),failure.reason);
  } catch (const std::bad_alloc&) {
    *status=kernel_failure(ErrorCode::ResourceExhausted,"FRQ staged allocation",
                           FailureReason::CapacityLimit);
  } catch (...) {
    *status=kernel_failure(ErrorCode::OperationFailed,"FRQ staged failure");
  }
}
inline OperationProgram lower(const OperationLowering& q) {
  const auto& s=*static_cast<const Frequency*>(q.static_state);
  const auto n=s.dims[0];
  OperationProgramBuilder b(q);
  ResourceLimits limits;limits.maximum_work=q.limits.maximum_work;
  ResourceBudget budget(limits);
  const auto plane_schema=std::make_shared<SchemaTemplate>(
      generation::tensor_schema(ElementType::Float64,{n,n,6},false,true));
  const auto root_schema=std::make_shared<SchemaTemplate>(
      generation::tensor_schema(ElementType::Float64,{n/2,6},false,true));
  const auto index_schema=std::make_shared<SchemaTemplate>(
      generation::tensor_schema(ElementType::Int64,{n}));
  const auto grid=take(TileGrid::from_schema(*plane_schema,q.tile_extent,0));
  const auto roots_grid=take(TileGrid::from_schema(*root_schema,q.tile_extent,0));
  const auto indices_grid=take(TileGrid::from_schema(*index_schema,q.tile_extent,0));
  const auto rows_value=take(b.add_temporary(plane_schema));
  const auto columns_value=take(b.add_temporary(plane_schema));
  const auto roots_value=take(b.add_temporary(root_schema));
  const auto indices_value=take(b.add_temporary(index_schema));
  const auto constants=take(b.add_task("frq-ball-constants",contract,{},
                                     {scratch(n,0),0,0}));
  std::vector<physical::Id> root_tiles,index_tiles;
  for (auto tile:take(sdk::tiles_for_region(budget,roots_grid,
                                           Region::whole({n/2,6}))))
    root_tiles.push_back(take(b.write_temporary(constants,roots_value,0,tile)));
  for (auto tile:take(sdk::tiles_for_region(budget,indices_grid,
                                           Region::whole({n}))))
    index_tiles.push_back(take(b.write_temporary(constants,indices_value,0,tile)));
  const auto bind_constants=[&](physical::Id task) {
    for (auto tile:root_tiles)check(b.read_tile(task,tile,2));
    for (auto tile:index_tiles)check(b.read_tile(task,tile,3));
  };
  const auto blocks=(n+q.tile_extent-1)/q.tile_extent;
  const auto band_blocks=(blocks+3)/4;
  std::map<TileIndex,physical::Id> row_tiles,column_tiles;
  for (std::uint64_t block=0;block<blocks;block+=band_blocks) {
    const auto start=block*q.tile_extent;
    const auto count=std::min(n-start,band_blocks*q.tile_extent);
    const auto task=take(b.add_task("frq-ball-rows",contract,{},
                                   {scratch(n,count),0,0}));
    auto& params=b.parameters(task);
    for (auto value:{start,count})
      for (unsigned i=0;i<8;++i)params.push_back(value>>(8*i));
    for (unsigned p=0;p<2;++p) {
      const auto input_grid=take(TileGrid::from_schema(
          *q.inputs[p].result_schema,q.tile_extent,0));
      for (auto tile:take(sdk::tiles_for_region(budget,input_grid,
                                                Region({{start,count},{0,n}}))))
        check(b.read_input(task,p,0,tile));
    }
    bind_constants(task);
    for (auto tile:take(sdk::tiles_for_region(budget,grid,
                            Region({{start,count},{0,n},{0,6}}))))
      row_tiles[tile]=take(b.write_temporary(task,rows_value,0,tile));
  }
  for (std::uint64_t block=0;block<blocks;block+=band_blocks) {
    const auto start=block*q.tile_extent;
    const auto count=std::min(n-start,band_blocks*q.tile_extent);
    const auto task=take(b.add_task("frq-ball-columns",contract,{},
                                   {scratch(n,count),0,0}));
    auto& params=b.parameters(task);
    for (auto value:{start,count})
      for (unsigned i=0;i<8;++i)params.push_back(value>>(8*i));
    bind_constants(task);
    for (auto tile:take(sdk::tiles_for_region(budget,grid,
                            Region({{0,n},{start,count},{0,6}})))) {
      check(b.read_tile(task,row_tiles.at(tile),4));
      column_tiles[tile]=take(b.write_temporary(task,columns_value,0,tile));
    }
  }
  const auto final=take(b.add_task("frq-ball-final",contract,{},
                                  {s.task_scratch(0,{}),0,0}));
  for (unsigned p=0;p<2;++p) {
    const auto input_grid=take(TileGrid::from_schema(
        *q.inputs[p].result_schema,q.tile_extent,0));
    for (auto tile:take(sdk::tiles_for_region(budget,input_grid,Region::whole({n,n}))))
      check(b.read_input(final,p,0,tile));
  }
  for (const auto& tile:column_tiles)check(b.read_tile(final,tile.second,5));
  for (unsigned p=0;p<q.requests.size();++p)
    for (const auto& member:q.requests[p].members)
      for (auto tile:member.second)check(b.write_output(final,p,member.first,tile));
  auto result=take(b.finish());
  for(auto& kernel:result.program.kernels)kernel.abi_version=9;
  check(physical::finalize_program(&result.program,q.limits));
  return result;
}
template <class Prepare>
Result<ps_prepared_kernel_v9> prepare(
    const physical::KernelDesc& desc, const physical::Placement& placement,
    const KernelPreparation& q, const ResourceSet& resources,
    const ps_native_gpu_service_v2* native, Prepare inner_prepare) {
  try {
    require(desc.contract == contract && desc.abi_version == 9 &&
                placement.backend == physical::Backend::Cpu &&
                placement.domain == physical::Domain::Host,
            "FRQ staged identity");
    auto owner=std::make_unique<Owner>();
    owner->state=std::allocate_shared<Frequency>(ResourceAllocator<Frequency>{},
                          *static_cast<const Frequency*>(q.static_state));
    auto& s=*owner->state;
    const auto n=s.dims[0];
    require(s.dims.size()==2 && n==s.dims[1] && power2(n) && n>=1024,
            "FRQ staged shape");
    owner->symbol=desc.symbol;
    const bool constants=desc.symbol=="frq-ball-constants";
    const bool rows=desc.symbol=="frq-ball-rows";
    const bool columns=desc.symbol=="frq-ball-columns";
    const bool final=desc.symbol=="frq-ball-final";
    require(constants||rows||columns||final,"FRQ staged symbol");
    if(rows||columns) {
      require(q.task_parameters.size()==16,"FRQ staged band encoding");
      const auto decode=[&](unsigned offset) {
        std::uint64_t value=0;
        for(unsigned i=0;i<8;++i)
          value|=static_cast<std::uint64_t>(q.task_parameters[offset+i])<<(8*i);
        return value;
      };
      owner->start=decode(0);owner->count=decode(8);
      require(owner->start<n && owner->count>0 && owner->count<=n-owner->start &&
                  owner->start%q.tile_extent==0 &&
                  (owner->start+owner->count==n || owner->count%q.tile_extent==0),
              "FRQ staged band bounds");
    } else require(q.task_parameters.empty(),"FRQ staged empty encoding");
    require(q.scratch==std::array<std::uint64_t,3>{
                final?s.task_scratch(0,{}):scratch(n,owner->count),0,0},
            "FRQ staged scratch");
    std::uint64_t root_count=0,index_count=0,plane_count=0,write_count=0;
    std::set<std::pair<physical::Id,TileIndex>> unique_reads;
    std::set<std::tuple<physical::Id,unsigned,TileIndex>> unique_writes;
    const auto plane_check=[&](const KernelView& v,bool write) {
      check(physical::validate_layout(v.layout,v.bytes,write));
      require(v.domain==physical::Domain::Host &&
                  v.layout.element_type==ElementType::Float64 &&
                  v.layout.rank==3 && v.layout.shape[2]==6 &&
                  v.layout.origin[2]==0 && v.layout.strides[2]==8,
              "FRQ staged Ball plane layout");
      const auto schema=generation::tensor_schema(ElementType::Float64,{n,n,6},false,true);
      const auto grid=take(TileGrid::from_schema(schema,q.tile_extent,0));
      require(fixed_tensor::matches_region(v.layout,take(grid.region(v.tile_index))),
              "FRQ staged plane tile");
      if(rows&&write)
        require(v.layout.origin[0]>=owner->start &&
                    v.layout.origin[0]+v.layout.shape[0]<=owner->start+owner->count,
                "FRQ staged row band tile");
      if(columns)
        require(v.layout.origin[1]>=owner->start &&
                    v.layout.origin[1]+v.layout.shape[1]<=owner->start+owner->count,
                "FRQ staged column band tile");
      return v.layout.shape[0]*v.layout.shape[1];
    };
    owner->reader.program=&s;
    for(unsigned p=0;p<2;++p) {
      owner->reader.input_base.push_back(p);
      owner->reader.groups.push_back({p,0,take(TileGrid::from_schema(
          *q.inputs[p].result_schema,q.tile_extent,0)),s.type,{}});
    }
    for(unsigned i=0;i<q.reads.size();++i) {
      const auto& v=q.reads[i];
      require(unique_reads.insert({v.port,v.tile_index}).second,"FRQ staged duplicate read");
      if(v.port<2) {
        require(rows||final,"FRQ staged original input stage");
        require(i==owner->original_reads++,"FRQ staged original input prefix");
        auto& group=owner->reader.groups[v.port];
        require(v.member==0 && v.domain==physical::Domain::Host &&
                    v.layout.element_type==s.type &&
                    fixed_tensor::matches_region(v.layout,take(group.grid.region(v.tile_index))),
                "FRQ staged original input layout");
        group.slots.emplace_back(v.tile_index,i);
      } else if(v.port==2||v.port==3) {
        require(rows||columns,"FRQ staged constants read stage");
        const auto schema=v.port==2
             ? generation::tensor_schema(ElementType::Float64,{n/2,6},false,true)
             : generation::tensor_schema(ElementType::Int64,{n});
        const auto grid=take(TileGrid::from_schema(schema,q.tile_extent,0));
        require(v.member==0 && v.domain==physical::Domain::Host &&
                    v.layout.element_type==schema.tensors[0].descriptor.element_type &&
                    fixed_tensor::matches_region(v.layout,take(grid.region(v.tile_index))),
                "FRQ staged constants read layout");
        (v.port==2?root_count:index_count)+=v.layout.shape[0];
      } else {
        require((columns&&v.port==4)||(final&&v.port==5),"FRQ staged plane read stage");
        plane_count+=plane_check(v,false);
      }
    }
    for(auto& group:owner->reader.groups) {
      std::sort(group.slots.begin(),group.slots.end());
      if(rows||final) {
        const auto region=rows?Region({{owner->start,owner->count},{0,n}}):Region::whole({n,n});
        const auto expected=take(sdk::tiles_for_region(ResourceBudget{},group.grid,region));
        require(expected.size()==group.slots.size(),"FRQ staged original input coverage");
        unsigned i=0;
        for(auto tile:expected)
          require(tile==group.slots[i++].first,"FRQ staged original input tile");
      }
    }
    if(rows||columns)
      require(root_count==n/2 && index_count==n,"FRQ staged constants coverage");
    if(columns||final)
      require(plane_count==n*(final?n:owner->count),"FRQ staged plane read coverage");
    for(const auto& v:q.writes) {
      require(unique_writes.insert({v.port,v.layout.rank,v.tile_index}).second,"FRQ staged duplicate write");
      if(!final)require(v.port==physical::none && v.member==0,"FRQ staged private write");
      if(rows||columns)write_count+=plane_check(v,true);
      if(constants) {
        const auto schema=v.layout.rank==2
             ? generation::tensor_schema(ElementType::Float64,{n/2,6},false,true)
             : generation::tensor_schema(ElementType::Int64,{n});
        const auto grid=take(TileGrid::from_schema(schema,q.tile_extent,0));
        require(v.layout.element_type==schema.tensors[0].descriptor.element_type &&
                    v.domain==physical::Domain::Host &&
                    fixed_tensor::matches_region(v.layout,take(grid.region(v.tile_index))),
                "FRQ staged constants write");
        (v.layout.rank==2?root_count:index_count)+=v.layout.shape[0];
      }
    }
    if(constants) {
      require(q.reads.empty() && root_count==n/2 && index_count==n,"FRQ staged constants output coverage");
      s.tables=std::allocate_shared<TransformTables>(ResourceAllocator<TransformTables>{});
    }
    if(rows||columns)require(write_count==n*owner->count,"FRQ staged plane write coverage");
    if(final) {
      s.staged_input=true;s.selected=Region::whole({n,n});
      const std::vector<std::uint8_t> empty;
      KernelPreparation inner(q.operation,q.inputs,q.parameters,empty);
      inner.static_state=&s;inner.outputs=q.outputs;inner.tile_extent=q.tile_extent;
      inner.scratch=q.scratch;inner.writes=q.writes;
      inner.reads.assign(q.reads.begin(),q.reads.begin()+owner->original_reads);
      auto identity=desc;identity.symbol="generation";identity.contract="generation-unchecked-v1";
      owner->inner=take(inner_prepare(identity,placement,inner,resources,native));
    }
    ps_prepared_kernel_v9 result{};
    result.struct_size=sizeof(result);result.abi_version=9;
    result.context=owner.release();result.invoke_cpu=invoke;
    result.release=[](const void* p){delete static_cast<const Owner*>(p);};
    return Result<ps_prepared_kernel_v9>(result);
  } catch(const Status& failure) {
    return Result<ps_prepared_kernel_v9>(failure);
  }
}
}  // namespace staged_internal
