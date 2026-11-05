#pragma once

// A head probe borrows the ordinary camera shell. Restore exact built geometry,
// rather than feeding saved half-sizes through viewport scaling a second time.
template<class Box, class Cylinder, class Element, class History>
class ScopedCameraProbeState
{
    Box& box;
    Cylinder& cylinder;
    Element& element;
    History history;
    Fvector size;
    Fmatrix box_form, cylinder_form, element_form;
    float radius, height;
public:
    ScopedCameraProbeState(Box& b, Cylinder& c, Element& e, History h)
        : box(b), cylinder(c), element(e), history(h), radius(c.radius()), height(c.height())
    {
        box.get_size(size); box.get_local_form_bt(box_form);
        cylinder.get_local_form_bt(cylinder_form);
        element.GetGlobalTransformDynamic(&element_form);
    }
    ~ScopedCameraProbeState()
    {
        box.set_size(size); box.set_local_form_bt(box_form);
        cylinder.set_radius(radius); cylinder.set_height(height);
        cylinder.set_local_form_bt(cylinder_form);
        element.SetTransform(element_form, history);
    }
    ScopedCameraProbeState(const ScopedCameraProbeState&) = delete;
    ScopedCameraProbeState& operator=(const ScopedCameraProbeState&) = delete;
};
