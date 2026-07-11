# Property Values

Use `uepi_schema(action="asset_property"|"class_property")` before `asset.set_properties`. The Editor reflection schema reports C++ type, property class, edit/read-only/transient flags, nested struct schema, collection element types, object class constraints, and enum paths.

## JSON Encoding

| Unreal property | JSON value |
|---|---|
| Bool | `true` / `false` |
| Integer/float | JSON number |
| String, Name, Text | JSON string |
| Enum | enumerator name string or numeric value |
| Object | exact object path string or `null` |
| Soft object | soft object path string |
| Struct | object keyed by reflected field name |
| Array | array |
| Set | array; UE rehashes after write |
| Map | array of `{ "key": ..., "value": ... }` entries |

Object references must load and satisfy the reflected allowed class. Unsupported or read-only properties fail preflight/apply; values are not coerced through display text.

## Property Paths

Paths use reflected names separated by dots. Array indexes use brackets:

```text
MovementConfig.MaxSpeed
Stages[2].Duration
```

The current P0 path traversal supports nested structs and array elements. Whole arrays, sets, and maps can be replaced through their containing property. Before and after values are recorded in the transaction diff.

## Example

```json
{
  "type": "asset.set_properties",
  "params": {
    "asset": "/Game/Data/DA_Example.DA_Example",
    "writes": [
      {"path": "DisplayName", "mode": "replace", "value": {"type": "text", "value": "Wave"}},
      {"path": "BlendTime", "mode": "replace", "value": {"type": "double", "value": 0.2}},
      {"path": "Tags", "mode": "replace", "value": {"type": "array", "items": [{"type": "name", "value": "Gesture"}, {"type": "name", "value": "UpperBody"}]}}
    ]
  }
}
```

Class/soft-class and instanced-subobject construction are not claimed by this alpha unless Discover and Schema explicitly advertise them.
