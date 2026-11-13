"""Check the game-window captures against independent ray and interface calculations."""
import argparse
import json
import math
from pathlib import Path
from PIL import Image, ImageChops

def normalize(xyz):
    length = math.sqrt(sum(x*x for x in xyz))
    return tuple(x/length for x in xyz)

def dot(a, b):
    return sum(x*y for x, y in zip(a, b))

def add(a, b, scale):
    return tuple(x+scale*y for x, y in zip(a, b))

def refract(direction, facing, ratio):
    cosine = -dot(direction, facing)
    discriminant = 1-ratio*ratio*(1-cosine*cosine)
    if discriminant < 0:
        return None
    return tuple(ratio*v+(ratio*cosine-math.sqrt(discriminant))*n for v,n in zip(direction,facing))

def reflectance(ci, ct, ni, nt):
    return .5*(((ni*ci-nt*ct)/(ni*ci+nt*ct))**2+((nt*ci-ni*ct)/(nt*ci+ni*ct))**2)

def ray_through_plate(x, y):
    angle = math.radians(48)
    normal = (math.sin(angle),0,-math.cos(angle))
    origin=(0,0,-4)
    direction=normalize((((x+.5)/960*2-1)*1.05,(1-(y+.5)/540*2)*1.05*9/16,1.60))
    front=add(origin,direction,-dot(origin,normal)/dot(direction,normal))
    inside=refract(direction,normal,1/1.5)
    exit_point=add(front,inside,.55/-dot(inside,normal))
    outgoing=refract(inside,normal,1.5)
    assert max(abs(a-b) for a,b in zip(direction,outgoing)) < 1e-9
    return add(exit_point,outgoing,(3-exit_point[2])/outgoing[2])[0]

def verify(directory):
    images={s:Image.open(directory/(s+'.png')).convert('RGB') for s in ('baseline','air','clear-glass','glass')}
    assert all(im.size==(960,540) for im in images.values())
    # Crop strictly inside the window aperture, away from the opaque mounting.
    roi=(310,190,575,345)
    diff=ImageChops.difference(images['baseline'].crop(roi),images['air'].crop(roi))
    max_error=max(x[1] for x in diff.getextrema())
    assert max_error<=1, f'Air control differs from no pane by {max_error} levels'
    def orange_edge(im):
        for x in range(350,490):
            r,g,b=im.getpixel((x,270))
            if r-g>44 and g-b>20:
                return x
        raise AssertionError('Orange reference stripe missing')
    baseline=orange_edge(images['baseline'])
    measured=orange_edge(images['clear-glass'])
    predicted=next(x for x in range(350,490) if ray_through_plate(x,270)>-.67)
    assert abs(measured-predicted)<=1 and measured>baseline+5,(baseline,measured,predicted)
    # Where the stripe is white, selective absorption must reduce red more than blue.
    sample=(350,270)
    loss=[a-b for a,b in zip(images['clear-glass'].getpixel(sample),images['glass'].getpixel(sample))]
    assert loss[0]>loss[2]>=0,loss
    angle=math.radians(48);theta_t=math.asin(math.sin(angle)/1.5)
    return {'screen_pixels':[960,540], 'air_control_max_rgb_error_8bit':max_error,
            'orange_stripe_left_x_no_glass':baseline,'orange_stripe_left_x_clear_glass':measured,
            'measured_shift_pixels':measured-baseline,'predicted_left_x':predicted,
            'absorption_rgb_loss_at_350_270':loss,'centre_incidence_deg':48,
            'centre_refracted_deg':math.degrees(theta_t),
            'first_interface_reflectance_at_normal':reflectance(1,1,1,1.5),
            'first_interface_reflectance_at_48_deg':reflectance(math.cos(angle),math.cos(theta_t),1,1.5)}
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('captures',type=Path)
    args=p.parse_args();print(json.dumps(verify(args.captures),indent=2))
