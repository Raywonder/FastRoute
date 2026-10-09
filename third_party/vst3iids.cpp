// The VST3 interface identifiers FastRoute uses.
//
// Every VST3 interface header declares its own IID as a static constant and
// leaves the definition of `Interface::iid` to exactly one translation unit in
// whoever is doing the linking. The SDK's own build has a file like this one;
// `pluginterfaces` on its own does not carry it, because a host and a plugin
// use different subsets and neither wants the other's.
//
// So this is that file, listing what a host needs: the factory to enumerate
// with, the component and processor to run audio through, the controller to
// read parameters from, the view to show an editor in, and the message and
// stream types those hand back and forth.
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstunits.h"

namespace Steinberg {

DEF_CLASS_IID (IPlugView)
DEF_CLASS_IID (IPlugFrame)

namespace Vst {

DEF_CLASS_IID (IComponent)
DEF_CLASS_IID (IAudioProcessor)
DEF_CLASS_IID (IEditController)
DEF_CLASS_IID (IEditController2)
DEF_CLASS_IID (IComponentHandler)
DEF_CLASS_IID (IConnectionPoint)
DEF_CLASS_IID (IMessage)
DEF_CLASS_IID (IAttributeList)
DEF_CLASS_IID (IHostApplication)
DEF_CLASS_IID (IParameterChanges)
DEF_CLASS_IID (IParamValueQueue)
DEF_CLASS_IID (IUnitInfo)

}   // namespace Vst
}   // namespace Steinberg
