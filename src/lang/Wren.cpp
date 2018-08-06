//  Copyright (c) 2018 Hugo Amiard hugo.amiard@laposte.net
//  This software is provided 'as-is' under the zlib License, see the LICENSE.txt file.
//  This notice and the license may not be removed or altered from any source distribution.

#include <infra/Cpp20.h>
#ifndef MUD_CPP_20
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <vector>
#include <map>
#include <set>
#include <type_traits>
#endif

#ifdef MUD_MODULES
#include <stdlib.h>
module mud.lang;
#else
#include <proto/Proto.h>
#include <infra/NonCopy.h>
#include <refl/Meta.h>
#include <refl/Enum.h>
#include <refl/Sequence.h>
#include <obj/Any.h>
#include <infra/Vector.h>
#include <refl/System.h>
#include <obj/Dispatch.h>
#include <obj/DispatchDecl.h>
#include <infra/Global.h>
#include <lang/Types.h>
#include <lang/Wren.h>
#endif

extern "C"
{
#include <wren.h>

void wrenAssignVariable(WrenVM* vm, const char* module, const char* name,
						int value_slot);
}

#define MUD_WREN_DEBUG_DECLS

namespace mud
{
	void wren_error(WrenVM* vm, WrenErrorType type, const char* module, int line, const char* message)
	{
		UNUSED(vm);
		printf("ERROR: wren -> %s:%i %s\n", module, line, message);
	}

	void wren_print(WrenVM* vm, const char* text)
	{
		UNUSED(vm);
		printf("%s\n", text);
	}

	class FromWren : public Dispatch<void, WrenVM*, int, Var&>, public LazyGlobal<FromWren>
	{
	public:
		FromWren();
	};

	class ToWren : public Dispatch<void, WrenVM*, int>, public LazyGlobal<ToWren>
	{
	public:
		ToWren();
	};

	inline void read_sequence(WrenVM* vm, int slot, Type& sequence_type, Var& result);
	inline void push_sequence(WrenVM* vm, int slot, const Var& value);

	inline void read_enum(WrenVM* vm, int slot, Type& type, Var& result);
	inline void push_enum(WrenVM* vm, int slot, const Var& value);

	inline void push_null(WrenVM* vm, int slot);

	inline Call& cached_call(const Callable& callable)
	{
		static std::vector<object_ptr<Call>> call_table;
		if(callable.m_index >= call_table.size())
			call_table.resize(callable.m_index + 1);

		if(!call_table[callable.m_index])
			call_table[callable.m_index] = make_object<Call>(callable);
		return *call_table[callable.m_index];
	}

	std::vector<WrenHandle*> g_wren_types = std::vector<WrenHandle*>(c_max_types);
	std::vector<WrenHandle*> g_wren_classes = std::vector<WrenHandle*>(c_max_types);
	std::vector<WrenHandle*> g_wren_methods = std::vector<WrenHandle*>(c_max_types);
	std::vector<WrenHandle*> g_construct_handles;

	inline Ref userdata(WrenVM* vm, int slot)
	{
		return *static_cast<Ref*>(wrenGetSlotForeign(vm, slot));
	}

	inline Ref read_ref(WrenVM* vm, int slot)
	{
		WrenType slot_type = wrenGetSlotType(vm, slot);
		if(slot_type == WREN_TYPE_NULL) return Ref();
		else if(slot_type != WREN_TYPE_FOREIGN) return Ref();
		else return userdata(vm, slot);
	}

	inline void read_object(WrenVM* vm, int slot, Type& type, Var& result)
	{
		WrenType slot_type = wrenGetSlotType(vm, slot);
		if(slot_type == WREN_TYPE_NULL) result = Ref(type);
		else if(slot_type == WREN_TYPE_FOREIGN)
		{
			Ref object = userdata(vm, slot);
			if(object.m_type->is(type))
				result = object;
		}
	}

	inline Type& read_type(WrenVM* vm, int slot)
	{
		Ref ref = read_ref(vm, slot);
		return val<Type>(ref);
	}

	inline void read_value(WrenVM* vm, int index, Type& type, Var& result)
	{
		FromWren::me().dispatch(Ref(type), vm, index, result);
		if(result.none())
			printf("ERROR : wren -> reading wrong type %s expected %s\n", "", type.m_name);//lua_typename(vm, lua_type(vm, index)), type.m_name);
	}

	inline Ref alloc_object(WrenVM* vm, int slot, int class_slot, Type& type)
	{
		Ref* ref = static_cast<Ref*>(wrenSetSlotNewForeign(vm, slot, class_slot, sizeof(Ref) + meta(type).m_size));
		new (ref) Ref(ref + 1, type); return *ref;
	}

	inline Ref alloc_ref(WrenVM* vm, int slot, int class_slot, Ref source)
	{
		Ref* ref = static_cast<Ref*>(wrenSetSlotNewForeign(vm, slot, class_slot, sizeof(Ref)));
		new (ref) Ref(source); return *ref;
	}

	inline Ref alloc_object(WrenVM* vm, int slot, Type& type)
	{
		int class_slot = wrenGetSlotCount(vm);
		wrenEnsureSlots(vm, class_slot + 1);
		wrenSetSlotHandle(vm, class_slot, g_wren_classes[type.m_id]);
		return alloc_object(vm, slot, class_slot, type);
	}

	inline Ref alloc_ref(WrenVM* vm, int slot, Ref ref)
	{
		int class_slot = wrenGetSlotCount(vm);
		wrenEnsureSlots(vm, class_slot + 1);
		wrenSetSlotHandle(vm, class_slot, g_wren_classes[type(ref).m_id]);
		return alloc_ref(vm, slot, class_slot, ref);
	}

	inline void push_ref(WrenVM* vm, int slot, Ref object)
	{
		alloc_ref(vm, slot, object);
	}

	inline void push_object(WrenVM* vm, int slot, Ref value)
	{
		Ref ref = alloc_object(vm, slot, *value.m_type);
		copy_construct(ref, value);
	}

	inline void push_value(WrenVM* vm, int slot, Ref value)
	{
		if(!ToWren::me().check(value))
		{
			printf("ERROR : wren -> no dispatch to push type %s\n", value.m_type->m_name);
			return push_null(vm, slot);
		}
		return ToWren::me().dispatch(value, vm, slot);
	}

	inline void read(WrenVM* vm, int index, Var& value)
	{
		if(value.m_mode == REF && value.m_ref == Ref())
			value = read_ref(vm, index);
		else if(type(value).is<Type>())
			value = read_ref(vm, index);
		else if(is_sequence(type(value)))
			read_sequence(vm, index, type(value), value);
		else if(is_object(type(value)) || is_struct(type(value)))
			read_object(vm, index, type(value), value);
		else if(is_enum(type(value)))
			read_enum(vm, index, type(value), value);
		else
			read_value(vm, index, type(value), value);
	}

	inline void push(WrenVM* vm, int slot, const Var& var)
	{
		// @todo: what about automatic conversion as with visual scripts ? it might not belong here, maybe in read() ?
		// @todo: might want a case for is_complex() before is_object() ?
		if(var.none() || var.null())
			return push_null(vm, slot);
		else if(is_sequence(type(var)))
			return push_sequence(vm, slot, var);
		else if(is_object(type(var)))
			return push_ref(vm, slot, var.m_ref);
		else if(is_struct(type(var)))
			return push_object(vm, slot, var.m_ref);
		else if(is_enum(type(var)))
			return push_enum(vm, slot, var);
		else
			return push_value(vm, slot, var.m_ref);
	}

	inline bool read_params(WrenVM* vm, const Param* params, array<Var> vars, size_t first)
	{
		for(size_t i = 0; i < vars.m_count; ++i)
		{
			read(vm, int(first) + int(i), vars[i]);
			bool success = !vars[i].none();
			success &= params[i].nullable() || !vars[i].null();
#if 1
			if(!success)
			{
				printf("ERROR: wren -> wrong argument %s, expect type %s, got %s\n", params[i].m_name, type(params[i].m_value).m_name, type(vars[i]).m_name);
				return false;
			}
#endif
		}
		return true;
	}

	inline void call_cpp(WrenVM* vm, Call& call, size_t first, size_t num_arguments)
	{
		bool enough_arguments = num_arguments >= call.m_arguments.size() - call.m_callable->m_num_defaults;
		if(enough_arguments && read_params(vm, &call.m_callable->m_params[0], to_array(call.m_arguments, 0, num_arguments), first))
		{
			call();
			if(!call.m_result.none())
				push(vm, 0, call.m_result);
#ifdef MUD_LUA_DEBUG
			printf("Wren -> called %s\n", call.m_callable->m_name);
#endif
		}
		else
			printf("ERROR: wren -> %s wrong arguments\n", call.m_callable->m_name);
	}

	inline void call_cpp(WrenVM* vm, const Callable& callable, size_t first, size_t num_arguments)
	{
		Call& call = cached_call(callable);
		return call_cpp(vm, call, first, num_arguments);
	}

	inline void call_function(WrenVM* vm, size_t num_args)
	{
		const Callable& callable = val<Callable>(userdata(vm, 0));
		call_cpp(vm, callable,  1, num_args);
	}

	template <size_t num_args>
	void call_function_args(WrenVM* vm)
	{
		call_function(vm, num_args);
	}

	inline void call_method(WrenVM* vm, size_t num_args)
	{
		const Callable& callable = val<Callable>(read_ref(vm, 0));
		Ref object = read_ref(vm, 1);
		call_cpp(vm, callable, 2, num_args);
	}

	template <size_t num_args>
	void call_method_args(WrenVM* vm)
	{
		call_method(vm, num_args);
	}

	inline void call_wren(WrenVM* vm, WrenHandle* method, Ref object, array<Var> parameters, Var* result = nullptr)
	{
		push_ref(vm, 0, object);
		for(size_t i = 0; i < parameters.size(); ++i)
			push(vm, i + 1, parameters[i]);
		wrenCall(vm, method);
		if(result) read(vm, 0, *result);
	}

	inline void call_wren_virtual(WrenVM* vm, Method& method, Ref object, array<Var> parameters)
	{
		call_wren(vm, g_wren_methods[method.m_index], object, parameters);
	}

	inline void get_member(WrenVM* vm)
	{
		const Member& member = val<Member>(read_ref(vm, 0));
		Ref object = read_ref(vm, 1);
		Ref value = member.cast_get(object);
		push(vm, 0, value);
	}

	inline void set_member(WrenVM* vm)
	{
		const Member& member = val<Member>(read_ref(vm, 0));
		Ref object = read_ref(vm, 1);
		Var value = member.m_default_value;
		read(vm, 2, value);
		member.cast_set(object, value);
	}

	inline void get_static(WrenVM* vm)
	{
		const Static& member = val<Static>(read_ref(vm, 0));
		push(vm, 0, member.m_value);
	}

	inline void set_static(WrenVM* vm)
	{
		Ref ref = read_ref(vm, 0);
		Static& member = val<Static>(ref);
		Var result = member.m_value;
		read(vm, 1, result);
		assign(member.m_value, result);
	}

	inline void construct(WrenVM* vm)
	{
		const Constructor* constructor = &val<Constructor>(read_ref(vm, 1));
		if(!constructor) return;
		Call& construct = cached_call(*constructor);
		if(read_params(vm, &construct.m_callable->m_params[1], to_array(construct.m_arguments, 1), 2))
			construct(alloc_object(vm, 0, 0, *constructor->m_object_type));
	}

	inline void construct_interface(WrenVM* vm)
	{
		const Constructor* constructor = &val<Constructor>(read_ref(vm, 0));
		if(!constructor) return;
		Call& construct = cached_call(*constructor);
		VirtualMethod virtual_method = [&](Method& method, Ref object, array<Var> args) { call_wren_virtual(vm, method, object, args); };
		construct.m_arguments.back() = var(virtual_method);
		if(read_params(vm, &construct.m_callable->m_params[1], to_array(construct.m_arguments, 1), 1))
			construct(alloc_object(vm, 0, 0, *constructor->m_object_type));
	}

	inline void destroy_function(WrenVM* vm)
	{
		Ref object = userdata(vm, 0);
		cls(object).m_destructor[0].m_call(object);
	}

	inline void register_enum(WrenVM* vm, string name, Type& type)
	{
		Enum& enu = mud::enu(type);

		string t = "    ";

		string members;
		for(size_t i = 0; i < enu.m_names.size(); ++i)
		{
			members += t + "static " + string(enu.m_names[i]) + " { " + to_string(enu.m_indices[i]) + " }\n";
		}

		string decl;
		decl += "class " + name + " {\n";
		decl += members;
		decl += "}\n";

		string module = meta(type).m_namespace->m_name != string("") ? meta(type).m_namespace->m_name : "main";

#ifdef MUD_WREN_DEBUG_DECLS
		printf("%s\n", decl.c_str());
#endif

		wrenInterpret(vm, module.c_str(), decl.c_str());
	}

	inline void register_class(WrenVM* vm, string name, Type& type)
	{
		if(type.is<Function>() || type.is<Type>() || type.is<Constructor>() || type.is<Method>() || type.is<Member>() || type.is<Static>()) return;
		if(type.is<Class>() || type.is<Creator>() || type.is<System>()) return;

		string constructors;
		string members;
		string methods;
		string statics;
		string init;

		string t = "    ";
		string c = type.m_name;

		init += t + t + "__type = Type.ref(\"" + c + "\")\n";

		for(Constructor& constructor : cls(type).m_constructors)
		{
			string n = "constructor" + to_string(constructor.m_index);
			string params = [&]() { if(constructor.m_params.size() == 1) return string("");  string params; for(Param& param : to_array(constructor.m_params, 1)) { params += param.m_name; params += ","; } params.pop_back(); return params; }();
			string paramsnext = params.empty() ? "" : ", " + params;

			init += t + t + "__" + n + " = Constructor.ref(\"" + c + "\", " + to_string(constructor.m_index) + ")\n";

			constructors += t + "construct new_impl(constructor" + paramsnext + ") {}\n";

			constructors += t + "static new(" + params + ") { new_impl(__" + n + paramsnext + ") }\n";
		}

		for(Member& member : cls(type).m_members)
		{
			string n = string(member.m_name);

			init += t + t + "__" + n + " = Member.ref(\"" + c + "\", \"" + n + "\")\n";

			members += t + n + " { __" + n + ".get(this) }\n";
			if(member.is_mutable())
				members += t + n + "=(value) { __" + n + ".set(this, value) }\n";
		}

		for(Method& method : cls(type).m_methods)
		{
			string n = string(method.m_name);
			string params = [&]() { if(method.m_params.size() == 0) return string(""); string params; for(Param& param : method.m_params) { params += param.m_name; params += ","; } params.pop_back(); return params; }();
			string paramsnext = params.empty() ? "" : ", " + params;

			init += t + t + "__" + n + " = Method.ref(\"" + c + "\", \"" + n + "\")\n";

			methods += t + n + "(" + params + ") { __" + n + ".call(this" + paramsnext + ") }\n";
		}

		for(Operator& op : cls(type).m_operators)
		{
			init += t + t + "__" + op.m_name + " = Operator.ref(\"" + op.m_name + "\", \"" + op.m_type->m_name + "\")\n";

			methods += t + op.m_sign + "(other) { __" + op.m_name + ".call(this, other) }\n";
		}

		for(Static& static_member : cls(type).m_static_members)
		{
			string n = string(static_member.m_name);

			init += t + t + "__" + n + " = Static.ref(\"" + c + "\", \"" + n + "\")\n";

			statics += t + "static " + n + " { __" + n + ".get() }\n";
			statics += t + "static " + n + "=(value) { __" + n + ".set(value) }\n";
		}

		string decl;
		decl += "foreign class " + name + " {\n";
		decl += "\n";
		decl += constructors;
		decl += "\n";
		decl += members;
		decl += "\n";
		decl += methods;
		decl += "\n";
		decl += statics;
		decl += "\n";
		decl += t + "static init() {\n";
		decl += init;
		decl += t + "}\n";
		decl += "\n";
		decl += "}\n";
		decl += "\n";
		decl += name + ".init()\n";

		string module = meta(type).m_namespace->m_name != string("") ? meta(type).m_namespace->m_name : "main";

#ifdef MUD_WREN_DEBUG_DECLS
		printf("%s\n", decl.c_str());
#endif

		WrenInterpretResult result = wrenInterpret(vm, module.c_str(), decl.c_str());

		if(result != WREN_RESULT_SUCCESS)
		{
			printf("ERROR: could not declare wren class %s\n", name.c_str());
			return;
		}

		wrenEnsureSlots(vm, 1);
		wrenGetVariable(vm, module.c_str(), name.c_str(), 0);
		g_wren_classes[type.m_id] = wrenGetSlotHandle(vm, 0);
	}

	WrenForeignClassMethods bindForeignClass(WrenVM* vm, const char* module, const char* className)
	{
		UNUSED(vm); UNUSED(module);

		WrenForeignClassMethods methods = {};

		if(strcmp(className, "Function") == 0)
		{
			methods.allocate = [](WrenVM* vm)
			{
				const char* n = wrenGetSlotString(vm, 1);
				const char* f = wrenGetSlotString(vm, 2);
				Function* function = system().find_function(n, f);
				alloc_ref(vm, 0, 0, Ref(function));
			};
		}
		else if(strcmp(className, "Type") == 0)
		{
			methods.allocate = [](WrenVM* vm)
			{
				const char* name = wrenGetSlotString(vm, 1);
				Type* type = system().find_type(name);
				alloc_ref(vm, 0, 0, Ref(type));
				g_wren_types[type->m_id] = wrenGetSlotHandle(vm, 0);
			};
		}
		else if(strcmp(className, "Constructor") == 0)
		{
			methods.allocate = [](WrenVM* vm)
			{
				const char* c = wrenGetSlotString(vm, 1);
				size_t index = size_t(wrenGetSlotDouble(vm, 2));
				Type* type = system().find_type(c);
				const Constructor* constructor = &cls(*type).m_constructors[index];
				alloc_ref(vm, 0, 0, Ref(constructor));
			};
		}
		else if(strcmp(className, "Member") == 0)
		{
			methods.allocate = [](WrenVM* vm)
			{
				const char* c = wrenGetSlotString(vm, 1);
				const char* m = wrenGetSlotString(vm, 2);
				Type* type = system().find_type(c);
				Member* member = &cls(*type).member(m);
				alloc_ref(vm, 0, 0, Ref(member));
			};
		}
		else if(strcmp(className, "Static") == 0)
		{
			methods.allocate = [](WrenVM* vm)
			{
				const char* c = wrenGetSlotString(vm, 1);
				const char* m = wrenGetSlotString(vm, 2);
				Type* type = system().find_type(c);
				Static* member = &cls(*type).static_member(m);
				alloc_ref(vm, 0, 0, Ref(member));
			};
		}
		else if(strcmp(className, "Method") == 0)
		{
			methods.allocate = [](WrenVM* vm)
			{
				const char* c = wrenGetSlotString(vm, 1);
				const char* m = wrenGetSlotString(vm, 2);
				Type* type = system().find_type(c);
				Method* method = &cls(*type).method(m);
				alloc_ref(vm, 0, 0, Ref(method));
			};
		}
		else if(strcmp(className, "Operator") == 0)
		{
			methods.allocate = [](WrenVM* vm)
			{
				const char* f = wrenGetSlotString(vm, 1);
				const char* t = wrenGetSlotString(vm, 2);
				Type* type = system().find_type(t);
				Function* function = cls(*type).op(f).m_function;
				alloc_ref(vm, 0, 0, Ref(function));
			};
		}
		else if(strcmp(className, "VirtualConstructor") == 0)
		{
			methods.allocate = [](WrenVM* vm)
			{
				const char* c = wrenGetSlotString(vm, 1);
				Type* type = system().find_type(c);
				const Constructor* constructor = &cls(*type).m_constructors[0];
				alloc_ref(vm, 0, 0, Ref(constructor));
			};
		}
		else
		{
			methods.allocate = construct;
		}

		return methods;
	}

	WrenForeignMethodFn bindForeignMethod(WrenVM* vm, const char* module, const char* className, bool isStatic, const char* signature)
	{
		UNUSED(vm); UNUSED(module); UNUSED(isStatic);

		if(strcmp(className, "Function") == 0)
		{
			if(strcmp(signature, "call()") == 0)
				return call_function_args<0>;
			if(strcmp(signature, "call(_)") == 0)
				return call_function_args<1>;
			if(strcmp(signature, "call(_,_)") == 0)
				return call_function_args<2>;
			if(strcmp(signature, "call(_,_,_)") == 0)
				return call_function_args<3>;
			if(strcmp(signature, "call(_,_,_,_)") == 0)
				return call_function_args<4>;
			if(strcmp(signature, "call(_,_,_,_,_)") == 0)
				return call_function_args<5>;
			if(strcmp(signature, "call(_,_,_,_,_,_)") == 0)
				return call_function_args<6>;
			if(strcmp(signature, "call(_,_,_,_,_,_,_)") == 0)
				return call_function_args<7>;
			if(strcmp(signature, "call(_,_,_,_,_,_,_,_)") == 0)
				return call_function_args<8>;
			if(strcmp(signature, "call(_,_,_,_,_,_,_,_,_)") == 0)
				return call_function_args<9>;
		}

		if(strcmp(className, "Type") == 0)
		{
			if(strcmp(signature, "new(_)") == 0)
			return [](WrenVM* vm)
			{
				const char* name = wrenGetSlotString(vm, 1);
				Ref t = alloc_object(vm, 0, 0, type<Type>());
				Type* type = new (t.m_value) Type(name);
				g_wren_types[type->m_id] = wrenGetSlotHandle(vm, 0);
			};
		}

		if(strcmp(className, "Operator") == 0)
		{
			if(strcmp(signature, "call(_,_)") == 0)
				return call_function_args<2>;
		}

		if(strcmp(className, "Member") == 0)
		{
			if(strcmp(signature, "get(_)") == 0)
				return get_member;
			if(strcmp(signature, "set(_,_)") == 0)
				return set_member;
		}

		if(strcmp(className, "Static") == 0)
		{
			if(strcmp(signature, "get()") == 0)
				return get_static;
			if(strcmp(signature, "set(_)") == 0)
				return set_static;
		}

		if(strcmp(className, "Method") == 0)
		{
			if(strcmp(signature, "call(_)") == 0)
				return call_method_args<0>;
			if(strcmp(signature, "call(_,_)") == 0)
				return call_method_args<1>;
			if(strcmp(signature, "call(_,_,_)") == 0)
				return call_method_args<2>;
			if(strcmp(signature, "call(_,_,_,_)") == 0)
				return call_method_args<3>;
			if(strcmp(signature, "call(_,_,_,_,_)") == 0)
				return call_method_args<4>;
			if(strcmp(signature, "call(_,_,_,_,_,_)") == 0)
				return call_method_args<5>;
		}

		if(strcmp(className, "VirtualConstructor") == 0)
		{
			if(strcmp(signature, "call()") == 0)
				return construct_interface;
			if(strcmp(signature, "call(_)") == 0)
				return construct_interface;
		}

		// Unknown method.
		return NULL;
	}

	template <class T>
	inline void read_integer(WrenVM* vm, int slot, Var& result) // std::is_integral<T>::value
	{
		if(wrenGetSlotType(vm, slot) == WREN_TYPE_NUM)
			val<T>(result) = static_cast<T>(wrenGetSlotDouble(vm, slot));
	}

	template <class T>
	inline void read_number(WrenVM* vm, int slot, Var& result)
	{
		if(wrenGetSlotType(vm, slot) == WREN_TYPE_NUM)
			val<T>(result) = static_cast<T>(wrenGetSlotDouble(vm, slot));
	}

	inline void read_cstring(WrenVM* vm, int slot, Var& result)
	{
		if(wrenGetSlotType(vm, slot) == WREN_TYPE_STRING)
			val<const char*>(result) = wrenGetSlotString(vm, slot);
	}

	inline void read_string(WrenVM* vm, int slot, Var& result)
	{
		if(wrenGetSlotType(vm, slot) == WREN_TYPE_STRING)
			val<string>(result) = wrenGetSlotString(vm, slot);
	}

	inline void read_null(WrenVM* vm, int slot, Var& result)
	{
		if(wrenGetSlotType(vm, slot) == WREN_TYPE_NULL)
			result = Ref();
	}

	inline void read_enum(WrenVM* vm, int slot, Type& type, Var& result)
	{
		if(wrenGetSlotType(vm, slot) == WREN_TYPE_NUM)
			result = enum_value(type, size_t(wrenGetSlotDouble(vm, slot)));
	}

	inline void read_sequence(WrenVM* vm, int slot, Type& sequence_type, Var& result)
	{
		if(wrenGetSlotType(vm, slot) != WREN_TYPE_LIST)
			return;

		int count = wrenGetListCount(vm, slot);
		
		for(int i = 0; i < count; ++i)
		{
			Var element = meta(*cls(sequence_type).m_content).m_empty_var();
			wrenGetListElement(vm, slot, i, slot + 1);
			read(vm, slot + 1, element);
			add_sequence(result, element);
		}
	}

	FromWren::FromWren()
	{
		function<int>([](Ref, WrenVM* vm, int slot, Var& result) { read_integer<int>(vm, slot, result); });
		function<uint32_t>([](Ref, WrenVM* vm, int slot, Var& result) { read_integer<uint32_t>(vm, slot, result); });
		function<float>([](Ref, WrenVM* vm, int slot, Var& result) { read_number<float>(vm, slot, result); });
		function<cstring>([](Ref, WrenVM* vm, int slot, Var& result) { read_cstring(vm, slot, result); });
		function<string>([](Ref, WrenVM* vm, int slot, Var& result) { read_string(vm, slot, result); });
		function<Id>([](Ref, WrenVM* vm, int slot, Var& result) { read_integer<Id>(vm, slot, result); });
		function<bool>([](Ref, WrenVM* vm, int slot, Var& result) { val<bool>(result) = wrenGetSlotBool(vm, slot); });

		function<Type>([](Ref, WrenVM* vm, int slot, Var& result) { result = read_ref(vm, slot); });
	}

	inline void push_null(WrenVM* vm, int slot)
	{
		wrenSetSlotNull(vm, slot);
	}

	inline void push_bool(WrenVM* vm, int slot, bool value)
	{
		wrenSetSlotBool(vm, slot, value);
	}

	inline void push_string(WrenVM* vm, int slot, const string& value)
	{
		wrenSetSlotString(vm, slot, value.c_str());
	}

	inline void push_cstring(WrenVM* vm, int slot, const char* value)
	{
		wrenSetSlotString(vm, slot, value);
	}

	template<typename T>
	inline void push_integer(WrenVM* vm, int slot, T value)
	{
		wrenSetSlotDouble(vm, slot, double(value));
	}

	template<typename T>
	inline void push_scalar(WrenVM* vm, int slot, T value)
	{
		wrenSetSlotDouble(vm, slot, double(value));
	}

	inline void push_dict(WrenVM* vm, int slot, const Var& value)
	{
		iterate_dict(value.m_ref, [=](Var key, Var element) {
		//	set_table(vm, key, element); });
		});
	}

	inline void push_sequence(WrenVM* vm, int slot, const Var& value)
	{
		wrenSetSlotNewList(vm, slot);
		size_t slots = wrenGetSlotCount(vm);
		wrenEnsureSlots(vm, slots + 1);

		size_t index = 1;
		iterate_sequence(value.m_ref, [&](Ref element) {
			push(vm, slots, element);
			wrenInsertInList(vm, slot, index, slots);
		});
	}

	inline void push_enum(WrenVM* vm, int slot, const Var& value)
	{
		wrenSetSlotDouble(vm, slot, double(enum_index(value.m_ref)));
	};

	ToWren::ToWren()
	{
		function<void>([](Ref, WrenVM* vm, int slot) { return push_null(vm, slot); });

		dispatch_branch<int>     (*this, [](int&      value, WrenVM* vm, int slot) { return push_integer(vm, slot, value); });
		dispatch_branch<uint16_t>(*this, [](uint16_t& value, WrenVM* vm, int slot) { return push_integer(vm, slot, value); });
		dispatch_branch<uint32_t>(*this, [](uint32_t& value, WrenVM* vm, int slot) { return push_integer(vm, slot, value); });
		dispatch_branch<uint64_t>(*this, [](uint64_t& value, WrenVM* vm, int slot) { return push_integer(vm, slot, value); });
		dispatch_branch<float>   (*this, [](float&    value, WrenVM* vm, int slot) { return push_scalar(vm, slot, value); });
		dispatch_branch<double>  (*this, [](double&   value, WrenVM* vm, int slot) { return push_scalar(vm, slot, value); });
		dispatch_branch<cstring> (*this, [](cstring   value, WrenVM* vm, int slot) { return push_cstring(vm, slot, value); });
		dispatch_branch<string>  (*this, [](string&   value, WrenVM* vm, int slot) { return push_string(vm, slot, value); });
		dispatch_branch<bool>    (*this, [](bool&     value, WrenVM* vm, int slot) { return push_bool(vm, slot, value); });
	}

	class WrenContext : public NonCopy
	{
	public:
		explicit WrenContext(std::vector<string> import_namespaces = {})
			: m_import_namespaces(import_namespaces)
		{
			WrenConfiguration config;
			wrenInitConfiguration(&config);

			config.bindForeignClassFn = bindForeignClass;
			config.bindForeignMethodFn = bindForeignMethod;
			config.errorFn = wren_error;
			config.writeFn = wren_print;

			m_vm = wrenNewVM(&config);

			for(size_t num_args = 0; num_args < 6; ++num_args)
			{
				string signature = "construct" + to_string(num_args) + "(";
				for(size_t i = 0; i < num_args; ++i)
					signature += "_" + (i == num_args - 1 ? string("") : string(","));
				signature += ")";

				g_construct_handles.push_back(wrenMakeCallHandle(m_vm, signature.c_str()));
			}

			string primitives =
				"foreign class Function {\n"
				"    construct ref(namespace, name) {}\n"
				"    \n"
				"    foreign call()\n"
				"    foreign call(a0)\n"
				"    foreign call(a0, a1)\n"
				"    foreign call(a0, a1, a2)\n"
				"    foreign call(a0, a1, a2, a3)\n"
				"    foreign call(a0, a1, a2, a3, a4)\n"
				"    foreign call(a0, a1, a2, a3, a4, a5)\n"
				"    foreign call(a0, a1, a2, a3, a4, a5, a6)\n"
				"    foreign call(a0, a1, a2, a3, a4, a5, a6, a7)\n"
				"    foreign call(a0, a1, a2, a3, a4, a5, a6, a7, a8)\n"
				"}\n"
				"\n"
				"foreign class Type {\n"
				"    foreign static new(name)\n"
				"    construct ref(name) {}\n"
				"}\n"
				"\n"
				"foreign class Constructor {\n"
				"    construct ref(class_name, index) {}\n"
				"}\n"
				"\n"
				"foreign class Member {\n"
				"    construct ref(class_name, member_name) {}\n"
				"    \n"
				"    foreign get(object)\n"
				"    foreign set(object, value)\n"
				"}\n"
				"\n"
				"foreign class Static {\n"
				"    construct ref(class_name, member_name) {}\n"
				"    \n"
				"    foreign get()\n"
				"    foreign set(value)\n"
				"}\n"
				"\n"
				"foreign class Method {\n"
				"    construct ref(class_name, method_name) {}\n"
				"    \n"
				"    foreign call(object)\n"
				"    foreign call(object, a0)\n"
				"    foreign call(object, a0, a1)\n"
				"    foreign call(object, a0, a1, a2)\n"
				"    foreign call(object, a0, a1, a2, a3)\n"
				"    foreign call(object, a0, a1, a2, a3, a4)\n"
				"}\n"
				"\n"
				"foreign class Operator {\n"
				"    construct ref(name, class_name) {}\n"
				"    \n"
				"    foreign call(a0, a1)\n"
				"}\n"
				"\n"
				"foreign class VirtualConstructor {\n"
				"    construct ref(class_name) {}\n"
				"    \n"
				"    foreign call()\n"
				"    foreign call(a0)\n"
				"}\n"
				;

			wrenInterpret(m_vm, "main", primitives.c_str());
		}

		~WrenContext()
		{
			assert(m_vm);
			wrenFreeVM(m_vm);
		}

		bool import_namespace(const string& path)
		{
			for(const string& name : m_import_namespaces)
				if(path == name)
					return true;
			return false;
		}

		array<cstring> namespace_path(Namespace& location, size_t shave_off = 0)
		{
			if(location.is_root())
				return array<cstring>{};
			if(import_namespace(location.m_path[0]))
				return location.m_path.size() == 1 ? array<cstring>{} : array<cstring>{ location.m_path.data() + 1, location.m_path.size() - 1 - shave_off };
			else
				return array<cstring>{ location.m_path.data(), location.m_path.size() - shave_off };
		}

		void register_namespace(Namespace& location)
		{
			if(location.is_root())
				return;
			string imports = "import \"main\" for Function, Type, Constructor, Member, Method, Static, Operator, VirtualConstructor\n";
			wrenInterpret(m_vm, location.m_name, imports.c_str());
		}

		string clean_name(cstring name)
		{
			string result = replace_all(replace_all(replace_all(name, "<", "_"), ">", ""), "*", "");
			for(string n : m_import_namespaces)
				result = replace_all(result, n + "::", "");
			return result;
		}

		void register_type(Type& type)
		{
			if(type.m_name == "Complex")
				int i = 0;
			string name = clean_name(type.m_name);
			if(is_class(type))
				register_class(m_vm, name, type);
			if(is_enum(type))
				register_enum(m_vm, name, type);
		}

		void register_function(Function& function)
		{
			if(as_operator(function))
				return;

			array<cstring> path = namespace_path(*function.m_namespace);

			string c = path.size() > 0 ? path[0] : "Module";
			string n = string(function.m_name);
			string parent = function.m_namespace->m_name;
			string params = [&]() { if(function.m_params.size() == 0) return string(""); string params; for(Param& param : function.m_params) { params += param.m_name; params += ","; } params.pop_back(); return params; }();
			string paramsnext = params.empty() ? "" : ", " + params;

			Functions& decls = m_function_decls[c];

			decls.functions += "    static " + n + "(" + params + ") { __" + n + ".call(" + params + ") }\n";

			decls.init += "        __" + n + " = Function.ref(\"" + parent + "\", \"" + n + "\")\n";
		}

		void declare_namespace(Namespace& location)
		{
			string c = location.m_name != string("") ? location.m_name : "Module";

			if(m_function_decls.find(c) == m_function_decls.end())
				return;

			Functions& decls = m_function_decls[c];

			string decl;
			decl += "class " + c + " {\n";
			decl += decls.functions;
			decl += "\n";
			decl += "    static init() {\n";
			decl += decls.init;
			decl += "    }\n";
			decl += "}\n";
			decl += "\n";
			decl += c + ".init()\n";

#ifdef MUD_WREN_DEBUG_DECLS
			printf("%s\n", decl.c_str());
#endif

			string parent = location.m_parent ? location.m_parent->m_name : "";
			string module = parent != "" ? parent : "main";
			wrenInterpret(m_vm, module.c_str(), decl.c_str());
		}

		std::vector<string> m_import_namespaces;

		struct Functions
		{
			string functions;
			string init;
		};

		std::map<string, Functions> m_function_decls;

		std::set<string> m_variables;

		WrenVM* m_vm;
	};
}


namespace mud
{
	WrenInterpreter::WrenInterpreter(bool import_symbols)
		: m_context(make_unique<WrenContext>(std::vector<string>{ "mud", "toy" }))
	{
		//g_lua_print_output = &m_output;
		if(import_symbols)
			this->declare_types();
	}

	WrenInterpreter::~WrenInterpreter()
	{}

	void WrenInterpreter::declare_types()
	{
		System& system = System::instance();

		//printf("INFO: Declaring wren Meta info\n");
		//system.dumpMetaInfo();

		for(Namespace& location : system.m_namespaces)
			m_context->register_namespace(location);

		for(Function* function : system.m_functions)
			m_context->register_function(*function);

		for(Namespace& location : system.m_namespaces)
			m_context->declare_namespace(location);

		for(Type* type : system.m_types)
			m_context->register_type(*type);
	}

	Var WrenInterpreter::get(cstring name, Type& type)
	{
		wrenEnsureSlots(m_context->m_vm, 1);
		wrenGetVariable(m_context->m_vm, "main", name, 0);
		Var result = Ref(&type);
		read(m_context->m_vm, 0, result);
		return result;
	}

	void WrenInterpreter::set(cstring name, Var value)
	{
		if(m_context->m_variables.find(name) == m_context->m_variables.end())
		{
			string create = "var " + string(name) + " = null";
			wrenInterpret(m_context->m_vm, "main", create.c_str());
			m_context->m_variables.insert(name);
		}
		wrenEnsureSlots(m_context->m_vm, 1);
		push(m_context->m_vm, 0, value);
		wrenAssignVariable(m_context->m_vm, "main", name, 0);
		Ref r = read_ref(m_context->m_vm, 0);
	}

	Var WrenInterpreter::getx(array<cstring> path, Type& type)
	{
		UNUSED(path); UNUSED(type);
		return Var();
	}

	void WrenInterpreter::setx(array<cstring> path, Var value)
	{
		UNUSED(path); UNUSED(value);
	}

	void WrenInterpreter::call(cstring code, Var* result)
	{
		wrenInterpret(m_context->m_vm, "main", code);
	}

	void WrenInterpreter::virtual_call(Method& method, Ref object, array<Var> args)
	{
		call_wren_virtual(m_context->m_vm, method, object, args);
	}
}